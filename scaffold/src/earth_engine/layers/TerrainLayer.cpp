#include "TerrainLayer.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../tiling/TilePlan.h"

#include <algorithm>

namespace earth_engine {

TerrainLayer::TerrainLayer(std::unique_ptr<TerrainProvider> provider,
                            std::unique_ptr<TileScheme> tileScheme)
    : id_(provider->id()),
      provider_(std::move(provider)),
      tileScheme_(std::move(tileScheme)),
      pendingQueue_(std::make_shared<PendingQueue>()) {}

TerrainLayer::~TerrainLayer() = default;

// ============================================================
// 高度采样
// ============================================================

float TerrainLayer::sampleHeight(double lngRad, double latRad) const {
    const TerrainTile* best = findBestTile(lngRad, latRad);
    if (!best) return 0.0f;
    return best->sampleHeight(lngRad, latRad);
}

const TerrainTile* TerrainLayer::findBestTile(double lngRad, double latRad) const {
    // 简单查找：遍历缓存，找到覆盖该坐标且 zoom 最高的 tile
    const TerrainTile* best = nullptr;
    int bestZoom = -1;
    for (const auto& [key, tile] : tileCache_) {
        if (!tile->valid()) continue;
        if (tile->bounds().contains(lngRad, latRad)) {
            if (tile->key().z > bestZoom) {
                bestZoom = tile->key().z;
                best = tile.get();
            }
        }
    }
    return best;
}

const TerrainTile* TerrainLayer::findBestTileForKey(const TileKey& targetKey) const {
    TileKey candidate = targetKey;
    while (true) {
        auto found = tileCache_.find(terrainCacheKey(candidate));
        if (found != tileCache_.end() && found->second && found->second->valid()) {
            return found->second.get();
        }
        if (candidate.z <= tileScheme_->minZoom()) {
            break;
        }
        candidate = TilePlanBuilder::parentKey(candidate);
    }
    return nullptr;
}

// ============================================================
// 帧更新
// ============================================================

void TerrainLayer::update(const FrameState& frameState) {
    if (!enabled_ || !visible_ || !frameState.camera) return;

    // 1. 处理后台线程完成的解码
    processPendingUploads();

    // 2. 计算可见瓦片
    tilePlan_ = TilePlanBuilder::compute(
        *frameState.camera, *tileScheme_,
        static_cast<double>(frameState.viewportWidthPixels),
        static_cast<double>(frameState.viewportHeightPixels));

    // 3. 请求缺失的瓦片（限制 zoom 范围到 provider 支持的范围）
    int requestsThisUpdate = 0;
    constexpr int kMaxTerrainRequestsPerUpdate = 8;
    for (const auto& key : tilePlan_.visibleTiles) {
        if (key.z < provider_->minZoom() || key.z > provider_->maxZoom()) continue;
        std::string cacheKey = terrainCacheKey(key);
        if (tileCache_.find(cacheKey) == tileCache_.end() &&
            requestedTiles_.find(cacheKey) == requestedTiles_.end()) {
            if (requestsThisUpdate >= kMaxTerrainRequestsPerUpdate) break;
            requestedTiles_.insert(cacheKey);
            loadTile(key);
            ++requestsThisUpdate;
        }
    }
}

void TerrainLayer::loadTile(const TileKey& key) {
    auto queue = pendingQueue_;  // shared_ptr copy protects against ~TerrainLayer
    CancellationToken token;

    provider_->requestTile(key, token,
        [queue, key](const TileKey& k, std::unique_ptr<DecodedHeightmap> hm) {
            if (!hm) return;
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->queue.push_back({k, std::move(hm)});
        });
}

void TerrainLayer::processPendingUploads() {
    std::deque<PendingUpload> batch;
    {
        std::lock_guard<std::mutex> lock(pendingQueue_->mutex);
        batch.swap(pendingQueue_->queue);
    }

    for (auto& item : batch) {
        std::string cacheKey = terrainCacheKey(item.key);

        auto tile = std::make_unique<TerrainTile>(
            item.key, *tileScheme_, std::move(item.heightmap));

        tileCache_[cacheKey] = std::move(tile);
        ++terrainGeneration_;
    }
}

std::string TerrainLayer::terrainCacheKey(const TileKey& key) const {
    return key.schemeId + "/" +
           std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" +
           std::to_string(key.y);
}

} // namespace earth_engine
