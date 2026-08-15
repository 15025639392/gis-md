#include "MvtVectorSource.h"

#include "MvtFeatureConverter.h"
#include "../core/async/AsyncSystem.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

namespace earth_engine {

MvtVectorSource::MvtVectorSource(Options options, Sinks sinks,
                                 std::shared_ptr<MvtTileFetchCache> tileCache,
                                 std::shared_ptr<ThreadPool> decodePool)
    : options_(std::move(options)),
      sinks_(std::move(sinks)),
      tileCache_(std::move(tileCache)),
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
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point a, Clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    lastStats_ = UpdateStats{};
    const auto tIngest = Clock::now();
    ingestInbox();

    const auto tTree = Clock::now();
    lastStats_.ingestMs = ms(tIngest, tTree);
    VectorTileTree::UpdateResult result =
        tree_.update(viewRect, cameraHeightMeters);
    const auto tDispatch = Clock::now();
    lastStats_.treeMs = ms(tTree, tDispatch);

    // 发缺瓦片请求(获取层单一化):fetch+解码+在途去重全在 tileCache_ ——
    // 与 drape/场共享实例时,同一块数据瓦跨消费方网络恰一次、解码恰一次。
    // 回调持 shared_ptr 收件箱,本对象析构后迟到安全;回调本体只做入箱
    // (mutex + push),无需再借 decodePool。树的 pending 集保证同一 key
    // 不重复走到这里,cache 的 inflight 兜跨消费方并发。
    if (tileCache_) {
        for (const TileKey& key : result.requestTiles) {
            std::weak_ptr<Inbox> weakInbox = inbox_;
            tileCache_->request(
                key, [key, weakInbox](std::shared_ptr<const MvtTile> tile) {
                    auto inbox = weakInbox.lock();
                    if (!inbox) return;
                    std::lock_guard<std::mutex> lock(inbox->mutex);
                    if (tile) {
                        inbox->decoded.emplace_back(key, std::move(tile));
                    } else {
                        inbox->failed.push_back(key);
                    }
                });
        }
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
        ++lastStats_.tessellateDispatched;
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

    const auto tCommit = Clock::now();
    lastStats_.dispatchMs = ms(tDispatch, tCommit);

    // ---- R* 换手(2026-08-15,V24 根修消缺陷④)----
    //
    // renderTiles 是树给出的精确覆盖。旧差分"出集即 drop、进集限速 commit"
    // 会造成旧的瞬间没、新的慢慢来 —— 整片换代最坏 16 帧空窗。改为
    // **置换单元原子换手**:被替换的旧瓦(占位者)只在其替换内容全部
    // 就绪的那一次 update 里,与替换内容的 commit 同帧退场。过渡期宁可
    // 旧内容多留几帧,决不出现空窗;同理,替换内容不提前上屏(占位者
    // 还在时提前 commit = 祖先/子瓦同框重影)。
    std::unordered_set<TileKey> renderSet(result.renderTiles.begin(),
                                          result.renderTiles.end());
    auto isAncestorOf = [](const TileKey& a, const TileKey& b) {
        if (a.schemeId != b.schemeId || a.z >= b.z) return false;
        const int d = b.z - a.z;
        return (b.x >> d) == a.x && (b.y >> d) == a.y;
    };
    auto overlaps = [&](const TileKey& a, const TileKey& b) {
        return isAncestorOf(a, b) || isAncestorOf(b, a);
    };

    // 1) 出集分类:整片离开视口(渲染集里没有任何覆盖它的替换者)→ 立即
    //    drop;有替换者 → 占位者,等单元完备再换手。
    std::vector<TileKey> toDrop;
    std::vector<TileKey> occupants;
    for (const TileKey& key : activeTiles_) {
        if (renderSet.count(key)) continue;
        bool hasReplacement = false;
        for (const TileKey& r : result.renderTiles) {
            if (overlaps(r, key)) {
                hasReplacement = true;
                break;
            }
        }
        (hasReplacement ? occupants : toDrop).push_back(key);
    }
    for (const TileKey& key : toDrop) {
        if (sinks_.drop) sinks_.drop(key);
        activeTiles_.erase(key);
        ++lastStats_.drops;
    }
    // 已镶好但已不在渲染集的网格直接丢弃,别占内存等一个不会来的 commit。
    for (auto it = readyMeshes_.begin(); it != readyMeshes_.end();) {
        it = renderSet.count(it->first) ? std::next(it)
                                        : readyMeshes_.erase(it);
    }

    // 2) 置换单元换手:单元 = 占位者 + 渲染集中与其重叠的全部瓦。单元内
    //    所有瓦 active 或网格就绪才动手:commit 缺席者 + drop 占位者,
    //    同一次 update 完成。预算(maxTileCommitsPerUpdate)拦上传尖刺,
    //    但单元不可拆 —— 每帧至少放行一个完备单元,宁可单帧超预算
    //    (最坏 4^kMaxDescendantStandinLevels 块)也不把换手劈成两帧重影。
    const size_t budget = options_.maxTileCommitsPerUpdate;
    size_t commits = 0;
    std::sort(occupants.begin(), occupants.end(),
              [](const TileKey& a, const TileKey& b) {
                  if (a.z != b.z) return a.z < b.z;
                  if (a.y != b.y) return a.y < b.y;
                  return a.x < b.x;
              });
    for (const TileKey& occ : occupants) {
        if (budget != 0 && commits >= budget) break;
        std::vector<TileKey> unit;
        bool ready = true;
        for (const TileKey& r : result.renderTiles) {
            if (!overlaps(r, occ)) continue;
            if (activeTiles_.count(r)) continue;  // 已上屏(共享占位者)
            if (!readyMeshes_.count(r)) {
                ready = false;
                break;
            }
            unit.push_back(r);
        }
        if (!ready) continue;  // 占位者继续顶着,不空窗
        for (const TileKey& r : unit) {
            auto it = readyMeshes_.find(r);
            if (it == readyMeshes_.end()) continue;  // 同帧被前一单元提走
            if (sinks_.commit) sinks_.commit(r, std::move(it->second));
            readyMeshes_.erase(it);
            activeTiles_.insert(r);
            ++commits;
        }
        if (sinks_.drop) sinks_.drop(occ);
        activeTiles_.erase(occ);
        ++lastStats_.drops;
    }

    // 3) 无占位者区域的新上屏(初次进入/离开后返回),原节流语义。
    //    与仍在场的占位者重叠的瓦不得提前上屏(重影),由其单元统一换手。
    for (const TileKey& key : result.renderTiles) {
        if (activeTiles_.count(key)) continue;
        auto it = readyMeshes_.find(key);
        if (it == readyMeshes_.end()) continue;  // 还没镶好
        if (budget != 0 && commits >= budget) {
            break;  // 余下的留到下帧,renderTiles 稳定时差分是幂等的
        }
        bool blocked = false;
        for (const TileKey& occ : activeTiles_) {
            if (!renderSet.count(occ) && overlaps(occ, key)) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        if (sinks_.commit) sinks_.commit(key, std::move(it->second));
        readyMeshes_.erase(it);
        activeTiles_.insert(key);
        ++commits;
    }
    lastStats_.commits = static_cast<int>(commits);
    lastStats_.commitMs = ms(tCommit, Clock::now());

    syncWorkTickets();
}

void MvtVectorSource::syncWorkTickets() {
    // Landing:fetch/decode 在途(tree_ 选中未到 = pending_)或 worker 镶嵌在途。
    // tree_.pendingCount 覆盖 request 派发→decode 回来这段窗口(此时 tessellating_
    // 与 readyMeshes_ 都空);ingest 与 dispatch 在同一次 update 内串行,无跨帧缝。
    const bool landing =
        tree_.pendingCount() > 0 || !tessellating_.empty();
    // Pumped:已镶好、等渲染线程 commit 的网格。停帧 = 永不 commit,故须出帧。
    const bool pumped = !readyMeshes_.empty();
    loadSlot_.reconcile(WorkLedger::Kind::Landing, "mvtVectorLoad", landing);
    commitSlot_.reconcile(WorkLedger::Kind::Pumped, "mvtVectorCommit", pumped);
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
    std::vector<std::pair<TileKey, std::shared_ptr<const MvtTile>>> decoded;
    std::vector<std::tuple<TileKey, FeatureTileMesh, uint64_t>> meshes;
    std::vector<TileKey> failed;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        decoded.swap(inbox_->decoded);
        meshes.swap(inbox_->meshes);
        failed.swap(inbox_->failed);
    }
    for (auto& [key, tile] : decoded) {
        tree_.provideShared(key, std::move(tile));
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
