#include "MvtVectorSource.h"

#include "MvtFeatureConverter.h"
#include "../core/async/AsyncSystem.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace earth_engine {

MvtVectorSource::MvtVectorSource(Options options, FeatureStore& store,
                                 FetchFn fetch, ThreadPool* decodePool)
    : options_(std::move(options)),
      fetch_(std::move(fetch)),
      decodePool_(decodePool),
      tree_(options_.tree),
      store_(store),
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
    for (const TileKey& key : result.requestTiles) {
        std::weak_ptr<Inbox> weakInbox = inbox_;
        ThreadPool* pool = decodePool_;
        fetch_(key, [key, weakInbox, pool](int statusCode,
                                           std::vector<uint8_t> body) {
            auto decodeAndDeliver = [key, weakInbox,
                                     body = std::move(body), statusCode]() {
                auto inbox = weakInbox.lock();
                if (!inbox) {
                    return;
                }
                MvtTile tile;
                bool ok = statusCode == 200 && !body.empty() &&
                          decodeMvtTile(body.data(), body.size(), tile);
                std::lock_guard<std::mutex> lock(inbox->mutex);
                if (ok) {
                    inbox->decoded.emplace_back(key, std::move(tile));
                } else {
                    inbox->failed.push_back(key);
                }
            };
            if (pool) {
                pool->enqueue(std::move(decodeAndDeliver));
            } else {
                decodeAndDeliver();
            }
        });
    }

    // 渲染集差分:进集激活,出集移除。激活按每帧要素预算摊销
    // (至少一整瓦片),未激活完的下帧继续——renderTiles 稳定时
    // 差分是幂等的,天然构成跨帧队列。
    std::unordered_set<TileKey> renderSet(result.renderTiles.begin(),
                                          result.renderTiles.end());
    std::vector<TileKey> toDeactivate;
    for (const auto& [key, ids] : activeTiles_) {
        if (!renderSet.count(key)) {
            toDeactivate.push_back(key);
        }
    }
    for (const TileKey& key : toDeactivate) {
        deactivateTile(key);
    }
    size_t activatedFeatures = 0;
    for (const TileKey& key : result.renderTiles) {
        if (activeTiles_.count(key)) {
            continue;
        }
        if (options_.maxActivationFeaturesPerUpdate != 0 &&
            activatedFeatures >= options_.maxActivationFeaturesPerUpdate) {
            break;
        }
        activatedFeatures += activateTile(key);
    }
}

void MvtVectorSource::ingestInbox() {
    std::vector<std::pair<TileKey, MvtTile>> decoded;
    std::vector<TileKey> failed;
    {
        std::lock_guard<std::mutex> lock(inbox_->mutex);
        decoded.swap(inbox_->decoded);
        failed.swap(inbox_->failed);
    }
    for (auto& [key, tile] : decoded) {
        tree_.provide(key, std::move(tile));
    }
    for (const TileKey& key : failed) {
        tree_.markFailed(key);
    }
}

size_t MvtVectorSource::activateTile(const TileKey& key) {
    const MvtTile* tile = tree_.loadedTile(key);
    if (tile == nullptr) {
        return 0;
    }
    std::vector<FeatureId>& ids = activeTiles_[key];
    for (const MvtLayer& layer : tile->layers) {
        if (!options_.includeLayers.empty() &&
            std::find(options_.includeLayers.begin(),
                      options_.includeLayers.end(),
                      layer.name) == options_.includeLayers.end()) {
            continue;
        }
        for (Feature& f : mvtLayerToFeatures(layer, key)) {
            ids.push_back(store_.addFeature(std::move(f)));
        }
    }
    return ids.size();
}

void MvtVectorSource::deactivateTile(const TileKey& key) {
    auto it = activeTiles_.find(key);
    if (it == activeTiles_.end()) {
        return;
    }
    for (FeatureId id : it->second) {
        store_.removeFeature(id);
    }
    activeTiles_.erase(it);
}

} // namespace earth_engine
