#include "TerrainLayer.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../tiling/TilePlan.h"
#include "../debug/PerfTimer.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cstdio>

namespace earth_engine {

namespace {

double rectangleCenterLongitude(const Rectangle& bounds) {
    double west = bounds.west();
    double east = bounds.east();
    if (bounds.crossesAntimeridian()) {
        east += glm::two_pi<double>();
    }
    double center = west + (east - west) * 0.5;
    if (center > glm::pi<double>()) {
        center -= glm::two_pi<double>();
    }
    return center;
}

double terrainRequestPriority(const TileScheme& scheme,
                              const TileKey& key,
                              const Vec3& cameraPosition,
                              const Vec3& cameraDirection,
                              bool hasInteractionFocus,
                              const Vec3& interactionFocusDirection) {
    const double directionLength = cameraDirection.length();
    if (directionLength <= 1e-6) {
        return static_cast<double>(key.z);
    }

    const Rectangle bounds = scheme.tileToRectangle(key);
    const double centerLng = rectangleCenterLongitude(bounds);
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;
    const Vec3 center = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 0.0));
    const Vec3 toTile = center - cameraPosition;
    const double distance = std::max(toTile.length(), 1.0);
    const Vec3 tileDirection = toTile / distance;
    const Vec3 viewDirection = cameraDirection / directionLength;
    const double centerPenalty =
        1.0 - std::clamp(tileDirection.dot(viewDirection), -1.0, 1.0);
    double anchorPenalty = centerPenalty;
    if (hasInteractionFocus && interactionFocusDirection.length() > 1e-6) {
        anchorPenalty = 1.0 - std::clamp(
            center.normalized().dot(interactionFocusDirection.normalized()),
            -1.0,
            1.0);
    }
    return centerPenalty * distance * 0.6 +
           anchorPenalty * distance * 0.4 +
           static_cast<double>(key.z) * 1000.0;
}

} // namespace

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

    const double updateStartMs = perf::nowMs();
    // 1. 处理后台线程完成的解码
    const double uploadStartMs = perf::nowMs();
    processPendingUploads();
    const double uploadMs = perf::nowMs() - uploadStartMs;

    const Vec3 cameraPosition = frameState.camera->position();
    const Vec3 cameraDirection = frameState.camera->direction();
    if (hasCameraState_) {
        const double positionDeltaMeters = cameraPosition.distanceTo(lastCameraPosition_);
        double directionDot = 1.0;
        if (cameraDirection.length() > 1e-6 && lastCameraDirection_.length() > 1e-6) {
            directionDot = std::clamp(
                cameraDirection.normalized().dot(lastCameraDirection_.normalized()),
                -1.0,
                1.0);
        }
        cameraMoving_ = positionDeltaMeters > 2.0 || directionDot < 0.99995;
    } else {
        hasCameraState_ = true;
        cameraMoving_ = false;
    }
    lastCameraPosition_ = cameraPosition;
    lastCameraDirection_ = cameraDirection;

    // 2. 计算可见瓦片（使用持久化 quad tree，避免每帧重建所有节点）
    if (!quadTree_) {
        quadTree_ = std::make_unique<TileQuadTree>();
    }
    const double planStartMs = perf::nowMs();
    bool reusedPlan = false;
    if (cameraMoving_ && lastPlanFrameId_ != 0 && (frameState.frameId % 2) != 0) {
        reusedPlan = true;
        tilePlan_.frameId = frameState.frameId;
    } else {
        tilePlan_ = quadTree_->compute(
            *frameState.camera, *tileScheme_,
            static_cast<double>(frameState.viewportWidthPixels),
            static_cast<double>(frameState.viewportHeightPixels));
        tilePlan_.frameId = frameState.frameId;
        lastPlanFrameId_ = frameState.frameId;
    }
    const double planMs = perf::nowMs() - planStartMs;

    // 3. 请求缺失的瓦片（限制 zoom 范围到 provider 支持的范围）
    const double requestStartMs = perf::nowMs();
    int requestsThisUpdate = 0;
    const size_t maxTerrainInflight = cameraMoving_ ? 96 : 128;
    const int maxTerrainRequestsPerUpdate = cameraMoving_ ? 2 : 4;
#ifdef __ANDROID__
    static int tileLogCount = 0;
    if (++tileLogCount <= 1) {
        for (const auto& key : tilePlan_.visibleTiles) {
            __android_log_print(ANDROID_LOG_INFO, "TerrainLayer",
                "  visible: %s/%d/%d/%d", key.schemeId.c_str(), key.z, key.x, key.y);
        }
    }
#endif
    std::vector<TileKey> requestCandidates;
    std::unordered_set<std::string> candidateSet;
    requestCandidates.reserve(tilePlan_.visibleTiles.size());
    for (const auto& key : tilePlan_.visibleTiles) {
        TileKey requestKey = key;
        while (requestKey.z > provider_->maxZoom()) {
            requestKey = TilePlanBuilder::parentKey(requestKey);
        }
        if (requestKey.z < provider_->minZoom()) continue;

        std::string cacheKey = terrainCacheKey(requestKey);
        if (tileCache_.find(cacheKey) == tileCache_.end() &&
            requestedTiles_.find(cacheKey) == requestedTiles_.end()) {
            if (!isTilePossiblyAvailable(requestKey)) continue;
            if (candidateSet.insert(cacheKey).second) {
                requestCandidates.push_back(requestKey);
            }
        }
    }
    std::stable_sort(requestCandidates.begin(), requestCandidates.end(),
        [&](const TileKey& lhs, const TileKey& rhs) {
            const double lhsPriority = terrainRequestPriority(
                *tileScheme_,
                lhs,
                cameraPosition,
                cameraDirection,
                frameState.hasInteractionFocus,
                frameState.interactionFocusDirection);
            const double rhsPriority = terrainRequestPriority(
                *tileScheme_,
                rhs,
                cameraPosition,
                cameraDirection,
                frameState.hasInteractionFocus,
                frameState.interactionFocusDirection);
            if (lhsPriority != rhsPriority) return lhsPriority < rhsPriority;
            if (lhs.z != rhs.z) return lhs.z < rhs.z;
            if (lhs.y != rhs.y) return lhs.y < rhs.y;
            return lhs.x < rhs.x;
        });

    for (const TileKey& requestKey : requestCandidates) {
        if (requestedTiles_.size() >= maxTerrainInflight) break;
        if (requestsThisUpdate >= maxTerrainRequestsPerUpdate) break;
        const std::string cacheKey = terrainCacheKey(requestKey);
        if (requestedTiles_.insert(cacheKey).second) {
            loadTile(requestKey);
            ++requestsThisUpdate;
        }
    }
    const double requestMs = perf::nowMs() - requestStartMs;

    char detail[256];
    std::snprintf(detail, sizeof(detail),
        "upload=%.2f tilePlan=%.2f request=%.2f visible=%zu candidates=%zu issued=%d cached=%d inflight=%zu empty=%zu moving=%d reusedPlan=%d focus=%d maxInflight=%zu",
        uploadMs,
        planMs,
        requestMs,
        tilePlan_.visibleTiles.size(),
        requestCandidates.size(),
        requestsThisUpdate,
        cachedTileCount(),
        requestedTiles_.size(),
        emptyTiles_.size(),
        cameraMoving_ ? 1 : 0,
        reusedPlan ? 1 : 0,
        frameState.hasInteractionFocus ? 1 : 0,
        maxTerrainInflight);
    perf::logTiming(frameState.frameId,
                    "TerrainLayer.update",
                    perf::nowMs() - updateStartMs,
                    detail);
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
    const double startMs = perf::nowMs();
#ifdef __ANDROID__
    static int ppuCount = 0;
    if (++ppuCount <= 3)
        __android_log_print(ANDROID_LOG_INFO, "TerrainLayer",
            "processPendingUploads #%d queue=%zu empty=%zu",
            ppuCount, pendingQueue_->queue.size(), pendingQueue_->emptyTiles.size());
#endif
    std::deque<PendingUpload> batch;
    size_t queueBefore = 0;
    size_t queueAfter = 0;
    size_t emptyBefore = 0;
    std::vector<std::string> emptyCompleted;
    {
        std::lock_guard<std::mutex> lock(pendingQueue_->mutex);
        queueBefore = pendingQueue_->queue.size();
        emptyBefore = pendingQueue_->emptyTiles.size();
        // Drain empty-tile markers from worker threads (cesium-native availability)
        emptyCompleted.assign(
            pendingQueue_->emptyTiles.begin(),
            pendingQueue_->emptyTiles.end());
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
        queueAfter = pendingQueue_->queue.size();
    }

    int uploaded = 0;
    for (const std::string& cacheKey : emptyCompleted) {
        requestedTiles_.erase(cacheKey);
    }
    for (auto& item : batch) {
        std::string cacheKey = terrainCacheKey(item.key);

        auto tile = std::make_unique<TerrainTile>(
            item.key, *tileScheme_, std::move(item.heightmap));

        tileCache_[cacheKey] = std::move(tile);
        requestedTiles_.erase(cacheKey);
        ++terrainGeneration_;
        ++uploaded;
    }

    char detail[160];
    std::snprintf(detail, sizeof(detail),
        "queue=%zu->%zu batch=%zu uploaded=%d emptyDrain=%zu cached=%d gen=%llu",
        queueBefore,
        queueAfter,
        batch.size(),
        uploaded,
        emptyBefore,
        cachedTileCount(),
        static_cast<unsigned long long>(terrainGeneration_));
    perf::logTiming(tilePlan_.frameId,
                    "TerrainLayer.processPendingUploads",
                    perf::nowMs() - startMs,
                    detail);
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
