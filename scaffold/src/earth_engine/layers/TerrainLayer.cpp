#include "TerrainLayer.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../tiling/TilePlan.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

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

const TerrainTile* TerrainLayer::findBestTileForBounds(const Rectangle& geoBounds) const {
    double tcLng = geoBounds.west() + geoBounds.width() * 0.5;
    double tcLat = geoBounds.south() + geoBounds.height() * 0.5;
    const TerrainTile* best = nullptr;
    int bestZoom = -1;
    for (const auto& [key, tile] : tileCache_) {
        if (!tile->valid()) continue;
        if (tile->bounds().contains(tcLng, tcLat)) {
            if (tile->key().z > bestZoom) {
                bestZoom = tile->key().z;
                best = tile.get();
            }
        }
    }
    if (!best && !tileCache_.empty()) {
        // Debug: print first tile bounds vs target
        auto& first = tileCache_.begin()->second;
#ifdef __ANDROID__
        __android_log_print(ANDROID_LOG_INFO, "TerrainLayer",
            "bounds mismatch: target(%.4f,%.4f) tile[%d/%d/%d](%.4f-%.4f,%.4f-%.4f) cache=%zu",
            tcLng, tcLat, first->key().z, first->key().x, first->key().y,
            first->bounds().west(), first->bounds().east(),
            first->bounds().south(), first->bounds().north(),
            tileCache_.size());
#else
        fprintf(stderr, "[TerrainLayer] bounds mismatch: target(%.4f,%.4f) "
            "tile[%d/%d/%d](%.4f-%.4f,%.4f-%.4f) cache=%zu\n",
            tcLng, tcLat, first->key().z, first->key().x, first->key().y,
            first->bounds().west(), first->bounds().east(),
            first->bounds().south(), first->bounds().north(),
            tileCache_.size());
#endif
    }
    return best;
}

const TerrainTile* TerrainLayer::findBestTileForKey(const TileKey& targetKey) const {
    // Try exact key match first (same scheme)
    auto found = tileCache_.find(terrainCacheKey(targetKey));
    if (found != tileCache_.end() && found->second && found->second->valid()) {
        return found->second.get();
    }
    // Cross-projection fallback: scan by geographic bounds
    return findBestTileForBounds(tileScheme_->tileToRectangle(targetKey));
}

// ============================================================
// 帧更新
// ============================================================

void TerrainLayer::update(const FrameState& frameState) {
    if (!enabled_ || !visible_ || !frameState.camera) return;

    // 1. 处理后台线程完成的解码
    processPendingUploads();

    // 2. 计算可见瓦片（使用持久化 quad tree，避免每帧重建所有节点）
    if (!quadTree_) {
        quadTree_ = std::make_unique<TileQuadTree>();
    }
    tilePlan_ = quadTree_->compute(
        *frameState.camera, *tileScheme_,
        static_cast<double>(frameState.viewportWidthPixels),
        static_cast<double>(frameState.viewportHeightPixels));

    // DEBUG: force-request known z=12 tiles for Chongqing area
    {
        TileKey k12{tileScheme_->id(), 12, 3259, 2721};
        tilePlan_.visibleTiles.push_back(k12);
        k12.y = 2722;
        tilePlan_.visibleTiles.push_back(k12);
    }

    // 3. 请求缺失的瓦片（限制 zoom 范围到 provider 支持的范围）
    int requestsThisUpdate = 0;
    constexpr int kMaxTerrainRequestsPerUpdate = 8;
#ifdef __ANDROID__
    static int tileLogCount = 0;
    if (++tileLogCount <= 1) {
        for (const auto& key : tilePlan_.visibleTiles) {
            __android_log_print(ANDROID_LOG_INFO, "TerrainLayer",
                "  visible: %s/%d/%d/%d", key.schemeId.c_str(), key.z, key.x, key.y);
        }
    }
#endif
    for (const auto& key : tilePlan_.visibleTiles) {
        if (key.z < provider_->minZoom() || key.z > provider_->maxZoom()) continue;
        std::string cacheKey = terrainCacheKey(key);
        if (tileCache_.find(cacheKey) == tileCache_.end() &&
            requestedTiles_.find(cacheKey) == requestedTiles_.end()) {
            // cesium-native availability: skip tiles known to be empty
            // Temporarily disabled for debugging
            // if (!isTilePossiblyAvailable(key)) continue;
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
    std::string cacheKey = terrainCacheKey(key);

    provider_->requestTile(key, token,
        [queue, key, cacheKey](const TileKey& k, std::unique_ptr<DecodedHeightmap> hm) {
            if (!hm) {
                // cesium-native availability: mark tile as empty so we don't
                // keep requesting a tile that has no data.
                std::lock_guard<std::mutex> lock(queue->mutex);
                queue->emptyTiles.insert(cacheKey);
                return;
            }
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->queue.push_back({k, std::move(hm)});
        });
}

void TerrainLayer::processPendingUploads() {
#ifdef __ANDROID__
    static int ppuCount = 0;
    if (++ppuCount <= 3)
        __android_log_print(ANDROID_LOG_INFO, "TerrainLayer",
            "processPendingUploads #%d queue=%zu empty=%zu",
            ppuCount, pendingQueue_->queue.size(), pendingQueue_->emptyTiles.size());
#endif
    std::deque<PendingUpload> batch;
    {
        std::lock_guard<std::mutex> lock(pendingQueue_->mutex);
        // Drain empty-tile markers from worker threads (cesium-native availability)
        emptyTiles_.insert(
            pendingQueue_->emptyTiles.begin(),
            pendingQueue_->emptyTiles.end());
        pendingQueue_->emptyTiles.clear();

        // Limit terrain tile processing per frame to avoid GPU buffer spikes
        constexpr size_t kMaxTerrainUploadsPerFrame = 2;
        while (!pendingQueue_->queue.empty() && batch.size() < kMaxTerrainUploadsPerFrame) {
            batch.push_back(std::move(pendingQueue_->queue.front()));
            pendingQueue_->queue.pop_front();
        }
    }

    for (auto& item : batch) {
        std::string cacheKey = terrainCacheKey(item.key);

        auto tile = std::make_unique<TerrainTile>(
            item.key, *tileScheme_, std::move(item.heightmap));

        tileCache_[cacheKey] = std::move(tile);
        ++terrainGeneration_;
    }
}

bool TerrainLayer::isTilePossiblyAvailable(const TileKey& key) const {
    // Walk the ancestor chain: if any ancestor has been confirmed empty,
    // this tile is also unavailable (cesium-native availability contract).
    TileKey candidate = key;
    while (true) {
        if (emptyTiles_.find(terrainCacheKey(candidate)) != emptyTiles_.end()) {
            return false;
        }
        if (candidate.z <= tileScheme_->minZoom()) break;
        candidate = TilePlanBuilder::parentKey(candidate);
    }
    return true;
}

std::string TerrainLayer::terrainCacheKey(const TileKey& key) const {
    return key.schemeId + "/" +
           std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" +
           std::to_string(key.y);
}

} // namespace earth_engine
