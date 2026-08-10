#include "MvtVectorSource.h"

#include "MvtFeatureConverter.h"
#include "../core/async/AsyncSystem.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace earth_engine {

MvtVectorSource::MvtVectorSource(Options options, Sinks sinks, FetchFn fetch,
                                 std::shared_ptr<ThreadPool> decodePool)
    : options_(std::move(options)),
      sinks_(std::move(sinks)),
      fetch_(std::move(fetch)),
      decodePool_(std::move(decodePool)),
      tree_(options_.tree),
      inbox_(std::make_shared<Inbox>()) {}

Rectangle MvtVectorSource::horizonViewRectangle(
    const Cartographic& cameraCarto, double ellipsoidMinRadiusMeters) {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kHalfPi = 0.5 * kPi;
    // 两侧地平线角相加 = 相机与要素互见(要素侧上界 10km,同
    // FeatureRenderLayer::visibleBucketKeys)
    constexpr double kMaxFeatureAltitudeMeters = 10000.0;
    const double r = ellipsoidMinRadiusMeters;
    const double camAltitude = std::max(0.0, cameraCarto.height());
    double theta =
        std::acos(std::clamp(r / (r + camAltitude), 0.0, 1.0)) +
        std::acos(std::clamp(r / (r + kMaxFeatureAltitudeMeters), 0.0, 1.0));
    // 近场覆盖上限:地平线角里的"要素侧 10km"项在低空恒占 ~3.2°,
    // 不加盖低空视口矩形仍是数百 km → 防爆闸把 zoom 压回粗档,细档
    // (z14)永远进不来。上限取 ~4×视高的地面距离(斜视 45° 屏幕内
    // 主要地面范围),高空时该上限大于地平线角、自然失效;远场粗档
    // 环带覆盖属后续(v1 远景无底图可接受——细档下远处道路本就不可辨)。
    constexpr double kMinCoverageMeters = 2000.0;
    theta = std::min(
        theta, std::max(4.0 * camAltitude, kMinCoverageMeters) / r);

    double south = std::max(cameraCarto.latitude() - theta, -kHalfPi);
    double north = std::min(cameraCarto.latitude() + theta, kHalfPi);
    if (cameraCarto.latitude() - theta <= -kHalfPi ||
        cameraCarto.latitude() + theta >= kHalfPi) {
        // 圆覆盖极点:经度全量
        return Rectangle(-kPi, south, kPi, north);
    }
    // 球冠经度包围界:Δλ = asin(sinθ / cos(lat₀))
    const double halfLngSpan = std::asin(std::clamp(
        std::sin(theta) / std::cos(cameraCarto.latitude()), 0.0, 1.0));
    if (halfLngSpan * 2.0 >= 2.0 * kPi) {
        return Rectangle(-kPi, south, kPi, north);
    }
    // 折回 [-π, π];跨反经线时 west > east(树侧拆段消化)
    double west = std::remainder(cameraCarto.longitude() - halfLngSpan, 2.0 * kPi);
    double east = std::remainder(cameraCarto.longitude() + halfLngSpan, 2.0 * kPi);
    return Rectangle(west, south, east, north);
}

void MvtVectorSource::update(const Rectangle& viewRect,
                             double cameraHeightMeters) {
    ingestInbox();

    VectorTileTree::UpdateResult result =
        tree_.update(viewRect, cameraHeightMeters);

    // 发缺瓦片请求。回调持 shared_ptr 收件箱,本对象析构后迟到安全。
    // 这一段**只解码**:解码产物 MvtTile 是样式无关的,归树的 LRU 缓存,
    // 瓦片重入视口时零重拉取。镶嵌是样式相关的派生物,拆成下面独立一段
    // 按需重做 —— 把两者揉进一个任务会让「重入」要么重拉网络、要么缓存
    // 一份样式一变就失效的网格。
    for (const TileKey& key : result.requestTiles) {
        std::weak_ptr<Inbox> weakInbox = inbox_;
        // pool 只持 weak(与 inbox 同一纪律,此前只做了一半):本回调在 curl
        // 线程异步送达 —— 取消也会补送 callback(-1) —— 可能晚于宿主拆除,
        // 裸指针 enqueue 已析构的线程池 = destroyed mutex abort(tombstone_22)。
        std::weak_ptr<ThreadPool> weakPool = decodePool_;
        const bool hadPool = decodePool_ != nullptr;
        fetch_(key, [key, weakInbox, weakPool, hadPool](
                        int statusCode, std::vector<uint8_t> body) {
            auto work = [key, weakInbox, body = std::move(body), statusCode]() {
                auto inbox = weakInbox.lock();
                if (!inbox) return;
                MvtTile tile;
                const bool ok = statusCode == 200 && !body.empty() &&
                                decodeMvtTile(body.data(), body.size(), tile);
                std::lock_guard<std::mutex> lock(inbox->mutex);
                if (ok) {
                    inbox->decoded.emplace_back(key, std::move(tile));
                } else {
                    inbox->failed.push_back(key);
                }
            };
            if (auto pool = weakPool.lock()) {
                pool->enqueue(std::move(work));
            } else if (!hadPool) {
                work();
            }
            // hadPool 且锁不上 = 宿主已拆除:丢弃(inbox 也已随之失效)。
        });
    }

    // 已解码但还没网格的渲染瓦片 → 派 worker 镶嵌。持共享所有权,树的 LRU
    // 淘汰不会把 worker 脚下的数据抽走。
    for (const TileKey& key : result.renderTiles) {
        if (activeTiles_.count(key) || readyMeshes_.count(key) ||
            tessellating_.count(key)) {
            continue;
        }
        std::shared_ptr<const MvtTile> tile = tree_.loadedTileShared(key);
        if (!tile || !sinks_.tessellate) continue;
        tessellating_.insert(key);
        std::weak_ptr<Inbox> weakInbox = inbox_;
        TessellateFn tessellate = sinks_.tessellate;
        std::vector<std::string> includeLayers = options_.includeLayers;
        std::vector<SourceLayerRule> rules = options_.layerRules;
        const uint64_t epoch = rulesEpoch_;
        auto work = [key, weakInbox, tile, tessellate, includeLayers,
                     rules, epoch]() {
            auto inbox = weakInbox.lock();
            if (!inbox) return;
            std::vector<Feature> features;
            for (const MvtLayer& layer : tile->layers) {
                if (!includeLayers.empty() &&
                    std::find(includeLayers.begin(), includeLayers.end(),
                              layer.name) == includeLayers.end()) {
                    continue;
                }
                // E2:层级规则。zoom 区间在**转换之前**判 —— 整层跳过时连
                // MVT→Feature 的转换都省了(P4 实测该转换 81ms/巨瓦),这正是
                // maplibre 把 layer minzoom/maxzoom 放在建桶前的理由。
                const SourceLayerRule* rule = nullptr;
                for (const SourceLayerRule& r : rules) {
                    if (r.layer == layer.name) { rule = &r; break; }
                }
                if (rule && (key.z < rule->minZoom || key.z > rule->maxZoom)) {
                    continue;
                }
                for (Feature& f : mvtLayerToFeatures(layer, key)) {
                    // 逐要素过滤在 worker 上跑(StyleFilter 不可变纯函数)。
                    if (rule && rule->filter &&
                        !rule->filter->matches(&f.properties, static_cast<double>(key.z))) {
                        continue;
                    }
                    features.push_back(std::move(f));
                }
            }
            FeatureTileMesh mesh = tessellate(key, std::move(features));
            std::lock_guard<std::mutex> lock(inbox->mutex);
            inbox->meshes.emplace_back(key, std::move(mesh), epoch);
        };
        if (decodePool_) decodePool_->enqueue(std::move(work));
        else work();
    }

    // 渲染集差分:进集 commit,出集 drop。镶嵌已在 worker 做完,这里只剩
    // 上传;闸拦的是上传成本,不再是镶嵌(见 Options 注释)。
    std::unordered_set<TileKey> renderSet(result.renderTiles.begin(),
                                          result.renderTiles.end());
    std::vector<TileKey> toDrop;
    for (const TileKey& key : activeTiles_) {
        if (!renderSet.count(key)) {
            toDrop.push_back(key);
        }
    }
    for (const TileKey& key : toDrop) {
        if (sinks_.drop) sinks_.drop(key);
        activeTiles_.erase(key);
    }
    // 已镶好但已不在渲染集的网格直接丢弃,别占内存等一个不会来的 commit。
    for (auto it = readyMeshes_.begin(); it != readyMeshes_.end();) {
        it = renderSet.count(it->first) ? std::next(it)
                                        : readyMeshes_.erase(it);
    }

    size_t commits = 0;
    for (const TileKey& key : result.renderTiles) {
        if (activeTiles_.count(key)) continue;
        auto it = readyMeshes_.find(key);
        if (it == readyMeshes_.end()) continue;  // 还没镶好
        if (options_.maxTileCommitsPerUpdate != 0 &&
            commits >= options_.maxTileCommitsPerUpdate) {
            break;  // 余下的留到下帧,renderTiles 稳定时差分是幂等的
        }
        if (sinks_.commit) sinks_.commit(key, std::move(it->second));
        readyMeshes_.erase(it);
        activeTiles_.insert(key);
        ++commits;
    }
}

void MvtVectorSource::setLayerRules(std::vector<SourceLayerRule> rules) {
    options_.layerRules = std::move(rules);
    // 网格是样式相关的派生物 —— 规则变了旧网格就不再有效。全部丢弃,下一次
    // update 会按新规则重镶。解码结果留在树的 LRU,故重镶零重拉取。
    for (const TileKey& key : activeTiles_) {
        if (sinks_.drop) sinks_.drop(key);
    }
    activeTiles_.clear();
    readyMeshes_.clear();
    // 已在途的镶嵌任务用的是旧规则,回来时直接丢:tessellating_ 清空后,
    // ingestInbox 认不出它们,readyMeshes_ 会收下 —— 故这里不能只清
    // tessellating_,得让 ingest 侧也识别。用代次戳最省事。
    ++rulesEpoch_;
    tessellating_.clear();
}

void MvtVectorSource::ingestInbox() {
    std::vector<std::pair<TileKey, MvtTile>> decoded;
    std::vector<std::tuple<TileKey, FeatureTileMesh, uint64_t>> meshes;
    std::vector<TileKey> failed;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        decoded.swap(inbox_->decoded);
        meshes.swap(inbox_->meshes);
        failed.swap(inbox_->failed);
    }
    for (auto& [key, tile] : decoded) {
        tree_.provide(key, std::move(tile));
    }
    for (auto& [key, mesh, epoch] : meshes) {
        tessellating_.erase(key);
        // 规则已变 → 这块是旧规则的产物,丢掉;下一次 update 会按新规则重派。
        if (epoch != rulesEpoch_) continue;
        readyMeshes_[key] = std::move(mesh);
    }
    for (const TileKey& key : failed) {
        tree_.markFailed(key);
    }
}

} // namespace earth_engine
