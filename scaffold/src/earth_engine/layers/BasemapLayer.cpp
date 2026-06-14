#include "BasemapLayer.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/Rectangle.h"
#include "../tiling/SurfaceTile.h"
#include "../tiling/TileSurface.h"
#include "../debug/PerfTimer.h"
#include "TerrainLayer.h"
#include "../terrain/QuantizedMeshParser.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace earth_engine {
namespace {

struct SurfaceGpuVertex {
    float position[3];   // 12 bytes, tile-relative ECEF
    float normal[3];     // 12 bytes, world-space normal
    float texcoord[2];   // 8 bytes
};

// SurfaceTileInstanceGpu removed — per-tile VBO rendering now.

float transitionOpacityForSurfaceDraw(const ImageryAttachment& attachment,
                                      float rawTransitionOpacity,
                                      bool cameraMoving) {
    const float clamped = std::clamp(rawTransitionOpacity, 0.0f, 1.0f);
    (void)attachment;
    if (cameraMoving) {
        return 1.0f;
    }
    return clamped;
}

std::vector<SurfaceGpuVertex> makeSurfaceGpuVertices(const SurfaceTileMesh& mesh,
                                                     const Vec3& origin) {
    std::vector<SurfaceGpuVertex> vertices;
    vertices.reserve(mesh.vertices.size());
    for (const SurfaceVertex& src : mesh.vertices) {
        SurfaceGpuVertex dst{};
        const Vec3 relative = src.positionEcef - origin;
        dst.position[0] = static_cast<float>(relative.x());
        dst.position[1] = static_cast<float>(relative.y());
        dst.position[2] = static_cast<float>(relative.z());
        // Normal: already in world space, stored directly
        Vec3 nrm = src.normalEcef;
        if (nrm.lengthSquared() > 0.0) nrm = nrm.normalized();
        else nrm = Ellipsoid::WGS84().geodeticSurfaceNormal(src.positionEcef);
        dst.normal[0] = static_cast<float>(nrm.x());
        dst.normal[1] = static_cast<float>(nrm.y());
        dst.normal[2] = static_cast<float>(nrm.z());
        dst.texcoord[0] = src.uv[0];
        dst.texcoord[1] = src.uv[1];
        vertices.push_back(dst);
    }
    return vertices;
}

Vec3 meshCentroid(const SurfaceTileMesh& mesh) {
    if (mesh.vertices.empty()) return Vec3::zero();

    Vec3 sum = Vec3::zero();
    for (const SurfaceVertex& vertex : mesh.vertices) {
        sum += vertex.positionEcef;
    }
    return sum / static_cast<double>(mesh.vertices.size());
}

int surfaceGridSizeForZoom(int zoom) {
    // Aligned with OpenGlobus RgbTerrain.gridSizeByZoom.
    // EmptyTerrain defaults are coarser (2 at zoom ≥ 8) but Mapbox
    // Terrain-RGB has 514×514 source data — enough for 32-64 grid at
    // medium-high zooms.
    static constexpr int kGridSizeByZoom[] = {
        64, 32, 16, 8, 8, 8, 16, 16, 16, 32, 32, 32, 32,
        32, 32, 64, 64, 64, 32, 32, 16, 8
    };
    const int index = std::clamp(
        zoom,
        0,
        static_cast<int>(sizeof(kGridSizeByZoom) / sizeof(kGridSizeByZoom[0])) - 1);
    return kGridSizeByZoom[index];
}

float transitionOpacityForTile(const TilePlan& plan, const TileKey& key) {
    for (const TileTransition& transition : plan.tileTransitions) {
        if (transition.key == key) {
            return std::clamp(transition.opacity, 0.0f, 1.0f);
        }
    }
    return 1.0f;
}

// writeRelativeCorner / writeRelativeCornerWithTerrain removed —
// per-tile VBO rendering uses makeSurfaceGpuVertices instead.

bool isDescendantOf(TileKey candidate, const TileKey& ancestor) {
    if (candidate.schemeId != ancestor.schemeId || candidate.z <= ancestor.z) {
        return false;
    }
    while (candidate.z > ancestor.z) {
        candidate = TilePlanBuilder::parentKey(candidate);
    }
    return candidate == ancestor;
}

bool tileTransitionsEqual(const std::vector<TileTransition>& lhs,
                          const std::vector<TileTransition>& rhs) {
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (!(lhs[i].key == rhs[i].key)) return false;
        if (lhs[i].opacity != rhs[i].opacity) return false;
        if (lhs[i].fadingNodeCount != rhs[i].fadingNodeCount) return false;
    }
    return true;
}

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

Vec3 ellipsoidSurfaceOriginForBounds(const Rectangle& bounds) {
    const double centerLng = rectangleCenterLongitude(bounds);
    const double centerLat = (bounds.south() + bounds.north()) * 0.5;
    return Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromRadians(centerLng, centerLat, 0.0));
}

double viewImportancePriority(const TileScheme& scheme,
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
    const double centerPenalty = 1.0 - std::clamp(tileDirection.dot(viewDirection), -1.0, 1.0);
    double anchorPenalty = centerPenalty;
    if (hasInteractionFocus && interactionFocusDirection.length() > 1e-6) {
        const Vec3 focusDirection = interactionFocusDirection.normalized();
        const Vec3 tileSurfaceDirection = center.normalized();
        anchorPenalty =
            1.0 - std::clamp(tileSurfaceDirection.dot(focusDirection), -1.0, 1.0);
    }

    // Cesium-native prioritizes load work by view-direction angle times distance.
    // During touch interaction, also bias toward the gesture anchor/focus so
    // the place under the user's fingers becomes clear before edges/horizon.
    return (centerPenalty * distance * 0.65) +
           (anchorPenalty * distance * 0.35) +
           static_cast<double>(key.z) * 1000.0;
}

void clearLayerTilePlanRetainingCapacity(LayerTilePlan& plan) {
    plan.layerId.clear();
    plan.providerId.clear();
    plan.frameId = 0;
    plan.zoom = 0;
    plan.minVisibleZoom = 0;
    plan.maxVisibleZoom = 0;
    plan.equalZoomApplied = false;
    plan.lodSizePixels = 0.0;
    plan.visibleTiles.clear();
    plan.tileTransitions.clear();
    plan.desiredTiles.clear();
    plan.requestTiles.clear();
    plan.renderTiles.clear();
    plan.fallbackTiles.clear();
    plan.kickedTiles.clear();
    plan.ancestorRetainedTiles.clear();
    plan.readyTileCount = 0;
    plan.parentFallbackReadyTileCount = 0;
    plan.missingTileCount = 0;
    plan.unsupportedTileCount = 0;
    plan.transitionTileCount = 0;
    plan.kickedTileCount = 0;
    plan.ancestorRetainedTileCount = 0;
    plan.quadtreeFadingNodeCount = 0;
    plan.quadtreeNeighborLinkCount = 0;
    plan.quadtreeNeighborBalancedTileCount = 0;
    plan.quadtreeRenderingNodeCount = 0;
    plan.quadtreeWalkthroughNodeCount = 0;
    plan.quadtreeNotRenderingNodeCount = 0;
    plan.quadtreeSelectionRenderedCount = 0;
    plan.quadtreeSelectionRefinedCount = 0;
    plan.quadtreeSelectionKickedCount = 0;
    plan.quadtreeSelectionAncestorMeetsSseCount = 0;
    plan.quadtreeCameraInsideNodeCount = 0;
    plan.quadtreeInFrustumNodeCount = 0;
    plan.quadtreeHorizonTangentPreservedCount = 0;
    plan.quadtreeEqualZoomSecondPassNodeCount = 0;
}

void recomputeRenderReadinessCounts(LayerTilePlan& plan) {
    plan.readyTileCount = 0;
    plan.parentFallbackReadyTileCount = 0;
    for (const RenderTileRef& renderTile : plan.renderTiles) {
        if (renderTile.source == TileRenderSource::Exact) {
            ++plan.readyTileCount;
        } else if (renderTile.source == TileRenderSource::ParentFallback) {
            ++plan.parentFallbackReadyTileCount;
        }
    }
}

} // namespace

BasemapLayer::BasemapLayer(std::unique_ptr<ImageryProvider> provider,
                            std::unique_ptr<TileScheme> tileScheme,
                            RenderDevice* renderDevice)
    : id_(provider->id()),
      provider_(std::move(provider)),
      tileScheme_(std::move(tileScheme)),
      renderDevice_(renderDevice),
      textureCache_(renderDevice, id_ + ":" + provider_->id(), 192 * 1024 * 1024),
      pendingQueue_(std::make_shared<PendingQueue>()) {}

BasemapLayer::~BasemapLayer() = default;

void BasemapLayer::update(const FrameState& frameState) {
    if (!visible_ || !frameState.camera) return;

    TilePlan plan = TilePlanBuilder::compute(
        *frameState.camera, *tileScheme_,
        static_cast<double>(frameState.viewportWidthPixels),
        static_cast<double>(frameState.viewportHeightPixels),
        tilePlan_.zoom);
    plan.frameId = frameState.frameId;

    applyPlan(plan,
              frameState.camera->position(),
              frameState.camera->direction(),
              frameState.hasInteractionFocus,
              frameState.interactionFocusDirection);
    loadMissingTiles();
}

void BasemapLayer::applyPlan(const TilePlan& plan,
                             const Vec3& cameraPosition,
                             const Vec3& cameraDirection,
                             bool hasInteractionFocus,
                             const Vec3& interactionFocusDirection) {
    if (!visible_) return;

    const double startMs = perf::nowMs();
    if (hasPreviousCameraState_) {
        const double positionDeltaMeters =
            cameraPosition.distanceTo(previousCameraPosition_);
        double directionDot = 1.0;
        if (cameraDirection.length() > 1e-6 &&
            previousCameraDirection_.length() > 1e-6) {
            directionDot = std::clamp(
                cameraDirection.normalized().dot(previousCameraDirection_.normalized()),
                -1.0,
                1.0);
        }
        cameraMoving_ = positionDeltaMeters > 2.0 || directionDot < 0.99995;
    } else {
        cameraMoving_ = false;
        hasPreviousCameraState_ = true;
    }
    previousCameraPosition_ = cameraPosition;
    previousCameraDirection_ = cameraDirection;
    lastCameraPosition_ = cameraPosition;
    lastCameraDirection_ = cameraDirection;
    hasInteractionFocus_ =
        hasInteractionFocus && interactionFocusDirection.length() > 1e-6;
    interactionFocusDirection_ = hasInteractionFocus_
        ? interactionFocusDirection.normalized()
        : Vec3::zero();
    const bool planChanged = isRenderAffectingPlanChange(plan);
    if (planChanged) {
        ++generation_;
        markLayerPlanDirty();
    }
    tilePlan_ = plan;

    const double uploadStartMs = perf::nowMs();
    processPendingUploads();
    const double uploadMs = perf::nowMs() - uploadStartMs;
    const double rebuildStartMs = perf::nowMs();
    rebuildLayerPlanIfNeeded();
    const double rebuildMs = perf::nowMs() - rebuildStartMs;

    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "layer=%s changed=%d dirty=%d rebuild=%.2f upload=%.2f visible=%zu desired=%zu render=%zu request=%zu",
        id_.c_str(),
        planChanged ? 1 : 0,
        layerPlanDirty_ ? 1 : 0,
        rebuildMs,
        uploadMs,
        tilePlan_.visibleTiles.size(),
        layerPlan_.desiredTiles.size(),
        layerPlan_.renderTiles.size(),
        layerPlan_.requestTiles.size());
    perf::logTiming(tilePlan_.frameId,
                    "BasemapLayer.applyPlan",
                    perf::nowMs() - startMs,
                    detail);
}

void BasemapLayer::loadMissingTiles() {
    const double startMs = perf::nowMs();
    constexpr double kRetryBackoffSec = 2.0;
    constexpr int kMaxRetries = 3;
    constexpr size_t kOpenGlobusLoadingBatchSize = 12;
    constexpr int kMaxIssuedRequestsPerFrame = 4;

    auto now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // 清理过期失败记录
    for (auto it = failedTiles_.begin(); it != failedTiles_.end(); ) {
        if (now - it->second.firstFailTime > 30.0) {
            it = failedTiles_.erase(it);
        } else {
            ++it;
        }
    }

    int issuedRequests = 0;
    for (const auto& key : layerPlan_.requestTiles) {
        if (requestedGeneration_.size() >= kOpenGlobusLoadingBatchSize) {
            break;
        }
        if (issuedRequests >= kMaxIssuedRequestsPerFrame) {
            break;
        }
        std::string ck = tileCacheKey(key);

        auto it = failedTiles_.find(ck);
        if (it != failedTiles_.end()) {
            if (it->second.retries >= kMaxRetries) continue;
            if (now - it->second.firstFailTime < kRetryBackoffSec) continue;
        }
        if (requestedGeneration_.find(ck) != requestedGeneration_.end()) {
            continue;
        }

        loadTile(key);
        ++issuedRequests;
    }

    char detail[160];
    std::snprintf(detail, sizeof(detail),
        "layer=%s candidates=%zu issued=%d inflight=%zu cached=%d",
        id_.c_str(),
        layerPlan_.requestTiles.size(),
        issuedRequests,
        requestedGeneration_.size(),
        cachedTileCount());
    perf::logTiming(tilePlan_.frameId,
                    "BasemapLayer.loadMissingTiles",
                    perf::nowMs() - startMs,
                    detail);
}

void BasemapLayer::loadTile(const TileKey& key) {
    auto queue = pendingQueue_;  // shared_ptr copy protects against ~BasemapLayer
    auto token = CancellationToken();
    const uint64_t generation = generation_;

    std::string ck = tileCacheKey(key);
    requestedGeneration_[ck] = generation;

    provider_->requestTile(key, token,
        [queue, key, generation](const TileKey& k, std::unique_ptr<DecodedImage> image) {
            std::lock_guard<std::mutex> lock(queue->mutex);
            queue->queue.push_back({k, generation, std::move(image)});
        });

    // 记录请求时间，用于失败检测
    auto now = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto& ft = failedTiles_[ck];
    if (ft.firstFailTime == 0.0) ft.firstFailTime = now;
    ft.retries++;
}

void BasemapLayer::processPendingUploads() {
    const double startMs = perf::nowMs();
    std::deque<PendingUpload> batch;
    size_t queueBefore = 0;
    size_t queueAfter = 0;
    {
        std::lock_guard<std::mutex> lock(pendingQueue_->mutex);
        queueBefore = pendingQueue_->queue.size();
        const size_t maxUploadsPerFrame = cameraMoving_ ? 1 : 2;
        while (!pendingQueue_->queue.empty() && batch.size() < maxUploadsPerFrame) {
            batch.push_back(std::move(pendingQueue_->queue.front()));
            pendingQueue_->queue.pop_front();
        }
        queueAfter = pendingQueue_->queue.size();
    }

    int uploaded = 0;
    int atlasUploaded = 0;
    int atlasFailed = 0;
    int currentFailures = 0;
    int staleDiscarded = 0;
    double textureCreateMs = 0.0;
    const double uploadBudgetMs = cameraMoving_ ? 2.0 : 6.0;
    size_t processedBatchItems = 0;
    for (auto& item : batch) {
        ++processedBatchItems;
        const std::string ck = tileCacheKey(item.key);
        auto clearRequestIfCurrent = [&]() {
            auto requested = requestedGeneration_.find(ck);
            if (requested != requestedGeneration_.end() &&
                requested->second == item.generation) {
                requestedGeneration_.erase(requested);
            }
        };
        if (!item.image) {
            clearRequestIfCurrent();
            ++currentFailures;
            continue;
        }
        if (!isCurrentPlanTileOrAncestor(item.key)) {
            clearRequestIfCurrent();
            ++staleDiscarded;
            continue;
        }
        auto& image = item.image;
        TextureDesc texDesc;
        texDesc.width = image->width;
        texDesc.height = image->height;
        texDesc.format = (image->channels == 4)
                             ? TextureDesc::Format::RGBA8
                             : TextureDesc::Format::RGB8;
        texDesc.data = image->pixels.data();
        texDesc.dataSize = image->pixels.size();
        texDesc.mipmap = true;
        texDesc.minFilter = TextureDesc::Filter::Linear;
        texDesc.magFilter = TextureDesc::Filter::Linear;
        texDesc.maxAnisotropy = 4.0f;
        texDesc.wrapS = TextureDesc::Wrap::Clamp;
        texDesc.wrapT = TextureDesc::Wrap::Clamp;

        const double textureStartMs = perf::nowMs();
        auto texture = renderDevice_->createTexture(texDesc);
        textureCreateMs += perf::nowMs() - textureStartMs;
        if (texture) {
            if (uploadImageryAtlasTile(item.key, *image)) {
                ++atlasUploaded;
            } else {
                ++atlasFailed;
            }
            // 成功上传 → 清除失败记录
            failedTiles_.erase(ck);
            clearRequestIfCurrent();
#ifdef __ANDROID__
            __android_log_print(ANDROID_LOG_INFO, "BasemapLayer",
                "Tile uploaded: %d/%d/%d %dx%d",
                item.key.z, item.key.x, item.key.y,
                image->width, image->height);
#endif
            textureCache_.put(item.key, std::move(texture));
            ++uploaded;
            markLayerPlanDirty();
        }
        if (textureCreateMs >= uploadBudgetMs) {
            break;
        }
    }
    if (processedBatchItems < batch.size()) {
        std::lock_guard<std::mutex> lock(pendingQueue_->mutex);
        for (size_t i = batch.size(); i > processedBatchItems; --i) {
            pendingQueue_->queue.push_front(std::move(batch[i - 1]));
        }
        queueAfter = pendingQueue_->queue.size();
    }

    size_t atlasEntries = 0;
    int atlasUsedSlots = 0;
    int atlasCapacitySlots = 0;
    int atlasReplacements = 0;
    for (const auto& [tileSize, atlas] : imageryAtlases_) {
        (void)tileSize;
        atlasEntries += atlas.entries.size();
        atlasUsedSlots += atlas.nextSlot;
        atlasCapacitySlots += atlas.columns * atlas.columns;
        atlasReplacements += atlas.replacements;
    }

    char detail[320];
    std::snprintf(detail, sizeof(detail),
        "layer=%s queue=%zu->%zu batch=%zu uploaded=%d atlasUploaded=%d atlasFailed=%d atlasEntries=%zu atlasSlots=%d/%d atlasReplace=%d failed=%d stale=%d uploadBudgetMs=%.1f textureCreate=%.2f cached=%d",
        id_.c_str(),
        queueBefore,
        queueAfter,
        batch.size(),
        uploaded,
        atlasUploaded,
        atlasFailed,
        atlasEntries,
        atlasUsedSlots,
        atlasCapacitySlots,
        atlasReplacements,
        currentFailures,
        staleDiscarded,
        uploadBudgetMs,
        textureCreateMs,
        cachedTileCount());
    perf::logTiming(tilePlan_.frameId,
                    "BasemapLayer.processPendingUploads",
                    perf::nowMs() - startMs,
                    detail);
}

bool BasemapLayer::isRenderAffectingPlanChange(const TilePlan& plan) const {
    if (layerPlanDirty_) return true;
    if (plan.zoom != tilePlan_.zoom) return true;
    if (plan.visibleTiles != tilePlan_.visibleTiles) return true;
    if (!tileTransitionsEqual(plan.tileTransitions, tilePlan_.tileTransitions)) return true;
    return false;
}

void BasemapLayer::markLayerPlanDirty() {
    layerPlanDirty_ = true;
}

void BasemapLayer::refreshLayerPlanFrameMetadata() {
    layerPlan_.frameId = tilePlan_.frameId;
    layerPlan_.zoom = tilePlan_.zoom;
    layerPlan_.minVisibleZoom = tilePlan_.minVisibleZoom;
    layerPlan_.maxVisibleZoom = tilePlan_.maxVisibleZoom;
    layerPlan_.equalZoomApplied = tilePlan_.equalZoomApplied;
    layerPlan_.lodSizePixels = tilePlan_.lodSizePixels;
    layerPlan_.quadtreeFadingNodeCount = tilePlan_.fadingNodeCount;
    layerPlan_.quadtreeNeighborLinkCount = tilePlan_.neighborLinkCount;
    layerPlan_.quadtreeNeighborBalancedTileCount =
        tilePlan_.neighborBalancedTileCount;
    layerPlan_.quadtreeRenderingNodeCount = tilePlan_.renderingNodeCount;
    layerPlan_.quadtreeWalkthroughNodeCount = tilePlan_.walkthroughNodeCount;
    layerPlan_.quadtreeNotRenderingNodeCount = tilePlan_.notRenderingNodeCount;
    layerPlan_.quadtreeSelectionRenderedCount = tilePlan_.selectionRenderedCount;
    layerPlan_.quadtreeSelectionRefinedCount = tilePlan_.selectionRefinedCount;
    layerPlan_.quadtreeSelectionKickedCount = tilePlan_.selectionKickedCount;
    layerPlan_.quadtreeSelectionAncestorMeetsSseCount =
        tilePlan_.selectionAncestorMeetsSseCount;
    layerPlan_.quadtreeCameraInsideNodeCount = tilePlan_.cameraInsideNodeCount;
    layerPlan_.quadtreeInFrustumNodeCount = tilePlan_.inFrustumNodeCount;
    layerPlan_.quadtreeHorizonTangentPreservedCount =
        tilePlan_.horizonTangentPreservedCount;
    layerPlan_.quadtreeEqualZoomSecondPassNodeCount =
        tilePlan_.equalZoomSecondPassNodeCount;
}

void BasemapLayer::rebuildLayerPlanIfNeeded() {
    if (!layerPlanDirty_) {
        refreshLayerPlanFrameMetadata();
        return;
    }
    rebuildLayerPlan();
    layerPlanDirty_ = false;
}

void BasemapLayer::rebuildLayerPlan() {
    const double startMs = perf::nowMs();
    double metadataMs = 0.0;
    double desiredMs = 0.0;
    double renderRefsMs = 0.0;
    double ancestorMs = 0.0;
    double kickingMs = 0.0;
    double evictMs = 0.0;

    const LayerTilePlan previousPlan = layerPlan_;
    {
        const double phaseStartMs = perf::nowMs();
        clearLayerTilePlanRetainingCapacity(layerPlan_);
        layerPlan_.layerId = id_;
        layerPlan_.providerId = provider_ ? provider_->id() : "";
        layerPlan_.frameId = tilePlan_.frameId;
        layerPlan_.zoom = tilePlan_.zoom;
        layerPlan_.minVisibleZoom = tilePlan_.minVisibleZoom;
        layerPlan_.maxVisibleZoom = tilePlan_.maxVisibleZoom;
        layerPlan_.equalZoomApplied = tilePlan_.equalZoomApplied;
        layerPlan_.lodSizePixels = tilePlan_.lodSizePixels;
        layerPlan_.quadtreeFadingNodeCount = tilePlan_.fadingNodeCount;
        layerPlan_.quadtreeNeighborLinkCount = tilePlan_.neighborLinkCount;
        layerPlan_.quadtreeNeighborBalancedTileCount =
            tilePlan_.neighborBalancedTileCount;
        layerPlan_.quadtreeRenderingNodeCount = tilePlan_.renderingNodeCount;
        layerPlan_.quadtreeWalkthroughNodeCount = tilePlan_.walkthroughNodeCount;
        layerPlan_.quadtreeNotRenderingNodeCount = tilePlan_.notRenderingNodeCount;
        layerPlan_.quadtreeSelectionRenderedCount = tilePlan_.selectionRenderedCount;
        layerPlan_.quadtreeSelectionRefinedCount = tilePlan_.selectionRefinedCount;
        layerPlan_.quadtreeSelectionKickedCount = tilePlan_.selectionKickedCount;
        layerPlan_.quadtreeSelectionAncestorMeetsSseCount =
            tilePlan_.selectionAncestorMeetsSseCount;
        layerPlan_.quadtreeCameraInsideNodeCount = tilePlan_.cameraInsideNodeCount;
        layerPlan_.quadtreeInFrustumNodeCount = tilePlan_.inFrustumNodeCount;
        layerPlan_.quadtreeHorizonTangentPreservedCount =
            tilePlan_.horizonTangentPreservedCount;
        layerPlan_.quadtreeEqualZoomSecondPassNodeCount =
            tilePlan_.equalZoomSecondPassNodeCount;
        layerPlan_.visibleTiles = tilePlan_.visibleTiles;
        layerPlan_.tileTransitions = tilePlan_.tileTransitions;
        layerPlan_.transitionTileCount += tilePlan_.fadingNodeCount;
        layerPlan_.desiredTiles.reserve(tilePlan_.visibleTiles.size());
        layerPlan_.requestTiles.reserve(tilePlan_.visibleTiles.size());
        layerPlan_.renderTiles.reserve(tilePlan_.visibleTiles.size());
        layerPlan_.fallbackTiles.reserve(tilePlan_.visibleTiles.size());
        metadataMs = perf::nowMs() - phaseStartMs;
    }

    std::unordered_set<TileKey> requestSet;
    requestSet.reserve(tilePlan_.visibleTiles.size());

    {
        const double phaseStartMs = perf::nowMs();
        std::unordered_set<TileKey> desiredSet;
        desiredSet.reserve(tilePlan_.visibleTiles.size());
        for (const auto& key : tilePlan_.visibleTiles) {
            TileKey desiredKey = key;
            while (provider_ && !provider_->supportsTile(desiredKey) &&
                   desiredKey.z > tileScheme_->minZoom()) {
                desiredKey = TilePlanBuilder::parentKey(desiredKey);
            }
            if (provider_ && !provider_->supportsTile(desiredKey)) {
                ++layerPlan_.unsupportedTileCount;
                continue;
            }
            if (desiredSet.insert(desiredKey).second) {
                layerPlan_.desiredTiles.push_back(desiredKey);
            }
        }
        desiredMs = perf::nowMs() - phaseStartMs;
    }

    {
        const double phaseStartMs = perf::nowMs();
        for (const auto& key : layerPlan_.desiredTiles) {
            const float lodTransitionOpacity = transitionOpacityForTile(tilePlan_, key);
            TileKey preferredTextureKey = key;

            TileKey coherentFallbackKey = preferredTextureKey;
            bool useCoherentParentFallback = false;

            if (textureCache_.contains(preferredTextureKey) &&
                preferredTextureKey.z > tileScheme_->minZoom()) {
                const TileKey parent = TilePlanBuilder::parentKey(preferredTextureKey);
                if (textureCache_.contains(parent)) {
                    for (const TileKey& sibling : layerPlan_.desiredTiles) {
                        if (sibling == key || sibling.z != key.z) continue;
                        TileKey siblingTextureKey = sibling;
                        while (provider_ && !provider_->supportsTile(siblingTextureKey) &&
                               siblingTextureKey.z > tileScheme_->minZoom()) {
                            siblingTextureKey = TilePlanBuilder::parentKey(siblingTextureKey);
                        }
                        if (TilePlanBuilder::parentKey(siblingTextureKey) != parent) continue;
                        if (!textureCache_.contains(siblingTextureKey)) {
                            coherentFallbackKey = parent;
                            useCoherentParentFallback = true;
                            break;
                        }
                    }
                }
            }

            if (useCoherentParentFallback) {
                layerPlan_.fallbackTiles.push_back(TileFallback{key, coherentFallbackKey});
                layerPlan_.renderTiles.push_back(RenderTileRef{
                    key,
                    coherentFallbackKey,
                    TileRenderSource::ParentFallback,
                    TileReadinessState::ParentFallback,
                    lodTransitionOpacity});
                ++layerPlan_.parentFallbackReadyTileCount;
                ++layerPlan_.transitionTileCount;
                continue;
            }

            if (textureCache_.contains(preferredTextureKey)) {
                layerPlan_.renderTiles.push_back(RenderTileRef{
                    key,
                    preferredTextureKey,
                    TileRenderSource::Exact,
                    TileReadinessState::Ready,
                    lodTransitionOpacity});
                ++layerPlan_.readyTileCount;
                continue;
            }

            TileKey fallbackKey = preferredTextureKey;
            const bool hasFallback =
                findFallbackTexture(preferredTextureKey, fallbackKey) != nullptr;
            if (hasFallback) {
                layerPlan_.fallbackTiles.push_back(TileFallback{key, fallbackKey});
                layerPlan_.renderTiles.push_back(RenderTileRef{
                    key,
                    fallbackKey,
                    TileRenderSource::ParentFallback,
                    TileReadinessState::ParentFallback,
                    1.0f});
                ++layerPlan_.parentFallbackReadyTileCount;
                ++layerPlan_.transitionTileCount;
            }

            TileKey requestKey = preferredTextureKey;
            const bool shouldRequestExact =
                hasFallback &&
                provider_ &&
                provider_->supportsTile(preferredTextureKey) &&
                !textureCache_.contains(preferredTextureKey);
            const bool shouldRequestFallbackChain =
                !hasFallback && findRequestTileForMissingTexture(preferredTextureKey, requestKey);
            if ((shouldRequestExact || shouldRequestFallbackChain) &&
                requestSet.insert(requestKey).second) {
                layerPlan_.requestTiles.push_back(requestKey);
            }
            if (!hasFallback) {
                ++layerPlan_.missingTileCount;
            }
        }
        sortRequestTilesByViewImportance();
        renderRefsMs = perf::nowMs() - phaseStartMs;
    }

    const double ancestorStartMs = perf::nowMs();
    applyAncestorMeetsSseFallback(previousPlan);
    ancestorMs = perf::nowMs() - ancestorStartMs;

    const double kickingStartMs = perf::nowMs();
    applyCesiumNativeKicking(previousPlan);
    kickingMs = perf::nowMs() - kickingStartMs;

    const double evictStartMs = perf::nowMs();
    evictUnusedSurfaceMeshes();
    evictMs = perf::nowMs() - evictStartMs;

    char detail[256];
    std::snprintf(detail, sizeof(detail),
        "metadata=%.2f desired=%.2f renderRefs=%.2f ancestor=%.2f kicking=%.2f evict=%.2f visible=%zu desiredCount=%zu render=%zu request=%zu cache=%zu",
        metadataMs,
        desiredMs,
        renderRefsMs,
        ancestorMs,
        kickingMs,
        evictMs,
        tilePlan_.visibleTiles.size(),
        layerPlan_.desiredTiles.size(),
        layerPlan_.renderTiles.size(),
        layerPlan_.requestTiles.size(),
        surfaceMeshCache_.size());
    perf::logTimingAtLeast(tilePlan_.frameId,
                           "BasemapLayer.rebuildLayerPlan",
                           perf::nowMs() - startMs,
                           8.0,
                           detail);
}

bool BasemapLayer::isCurrentDesiredTile(const TileKey& key) const {
    return std::find(layerPlan_.desiredTiles.begin(),
                     layerPlan_.desiredTiles.end(),
                     key) != layerPlan_.desiredTiles.end();
}

void BasemapLayer::sortRequestTilesByViewImportance() {
    if (!tileScheme_ || layerPlan_.requestTiles.size() < 2) {
        return;
    }
    std::stable_sort(layerPlan_.requestTiles.begin(),
                     layerPlan_.requestTiles.end(),
                     [&](const TileKey& lhs, const TileKey& rhs) {
        const double lhsPriority = viewImportancePriority(
            *tileScheme_, lhs, lastCameraPosition_, lastCameraDirection_,
            hasInteractionFocus_, interactionFocusDirection_);
        const double rhsPriority = viewImportancePriority(
            *tileScheme_, rhs, lastCameraPosition_, lastCameraDirection_,
            hasInteractionFocus_, interactionFocusDirection_);
        if (lhsPriority != rhsPriority) {
            return lhsPriority < rhsPriority;
        }
        if (lhs.z != rhs.z) return lhs.z < rhs.z;
        if (lhs.y != rhs.y) return lhs.y < rhs.y;
        return lhs.x < rhs.x;
    });
}

bool BasemapLayer::isCurrentPlanTileOrAncestor(const TileKey& key) const {
    if (std::find(layerPlan_.requestTiles.begin(),
                  layerPlan_.requestTiles.end(),
                  key) != layerPlan_.requestTiles.end()) {
        return true;
    }
    if (isCurrentDesiredTile(key)) return true;

    for (const TileKey& desired : layerPlan_.desiredTiles) {
        TileKey candidate = desired;
        while (candidate.z > tileScheme_->minZoom()) {
            candidate = TilePlanBuilder::parentKey(candidate);
            if (candidate == key) return true;
        }
    }
    return false;
}

Texture* BasemapLayer::findFallbackTexture(const TileKey& target, TileKey& textureKey) {
    TileKey candidate = target;
    while (candidate.z > tileScheme_->minZoom()) {
        candidate = TilePlanBuilder::parentKey(candidate);
        Texture* tex = textureCache_.get(candidate);
        if (tex) {
            textureKey = candidate;
            return tex;
        }
    }
    return nullptr;
}

bool BasemapLayer::buildRenderableRefForTile(const TileKey& target,
                                             float transitionOpacity,
                                             RenderTileRef& out) {
    if (provider_ && !provider_->supportsTile(target)) {
        return false;
    }

    if (textureCache_.contains(target)) {
        out = RenderTileRef{
            target,
            target,
            TileRenderSource::Exact,
            TileReadinessState::Ready,
            transitionOpacity};
        return true;
    }

    TileKey fallbackKey = target;
    if (findFallbackTexture(target, fallbackKey) != nullptr) {
        out = RenderTileRef{
            target,
            fallbackKey,
            TileRenderSource::ParentFallback,
            TileReadinessState::ParentFallback,
            transitionOpacity};
        return true;
    }

    return false;
}

void BasemapLayer::applyAncestorMeetsSseFallback(
    const LayerTilePlan& previousPlan) {
    if (previousPlan.renderTiles.empty() || layerPlan_.desiredTiles.empty()) {
        return;
    }

    std::unordered_set<TileKey> currentRenderTargets;
    currentRenderTargets.reserve(layerPlan_.renderTiles.size());
    for (const RenderTileRef& renderTile : layerPlan_.renderTiles) {
        currentRenderTargets.insert(renderTile.targetKey);
    }

    for (const TileKey& desired : layerPlan_.desiredTiles) {
        if (currentRenderTargets.find(desired) != currentRenderTargets.end()) {
            continue;
        }

        for (const RenderTileRef& previous : previousPlan.renderTiles) {
            if (!isDescendantOf(previous.targetKey, desired)) {
                continue;
            }
            if (!textureCache_.contains(previous.textureKey)) {
                continue;
            }
            if (!currentRenderTargets.insert(previous.targetKey).second) {
                continue;
            }

            layerPlan_.renderTiles.push_back(previous);
            layerPlan_.ancestorRetainedTiles.push_back(previous.targetKey);
        }
    }

    if (layerPlan_.ancestorRetainedTiles.empty()) return;

    layerPlan_.ancestorRetainedTileCount =
        static_cast<int>(layerPlan_.ancestorRetainedTiles.size());
    layerPlan_.transitionTileCount += layerPlan_.ancestorRetainedTileCount;
    recomputeRenderReadinessCounts(layerPlan_);
}

void BasemapLayer::applyCesiumNativeKicking(const LayerTilePlan& previousPlan) {
    if (layerPlan_.renderTiles.empty()) return;

    std::unordered_set<TileKey> previousRenderedTargets;
    previousRenderedTargets.reserve(previousPlan.renderTiles.size());
    for (const RenderTileRef& renderTile : previousPlan.renderTiles) {
        previousRenderedTargets.insert(renderTile.targetKey);
    }

    std::unordered_map<TileKey, std::vector<size_t>> childrenByParent;
    childrenByParent.reserve(layerPlan_.renderTiles.size());
    for (size_t i = 0; i < layerPlan_.renderTiles.size(); ++i) {
        const TileKey& target = layerPlan_.renderTiles[i].targetKey;
        if (target.z <= tileScheme_->minZoom()) continue;
        childrenByParent[TilePlanBuilder::parentKey(target)].push_back(i);
    }

    std::vector<bool> kicked(layerPlan_.renderTiles.size(), false);
    std::vector<RenderTileRef> parentRenderTiles;

    for (const auto& [parentKey, indices] : childrenByParent) {
        if (indices.empty()) continue;

        bool allAreExactReady = true;
        bool anyWereRenderedLastFrame = false;
        for (size_t index : indices) {
            const RenderTileRef& renderTile = layerPlan_.renderTiles[index];
            allAreExactReady =
                allAreExactReady &&
                renderTile.readiness == TileReadinessState::Ready &&
                renderTile.source == TileRenderSource::Exact;
            anyWereRenderedLastFrame =
                anyWereRenderedLastFrame ||
                previousRenderedTargets.find(renderTile.targetKey) !=
                    previousRenderedTargets.end();
        }

        if (allAreExactReady || anyWereRenderedLastFrame) {
            continue;
        }

        RenderTileRef parentRef;
        constexpr float opacity = 1.0f;
        if (!buildRenderableRefForTile(parentKey, opacity, parentRef)) {
            continue;
        }

        parentRenderTiles.push_back(parentRef);
        for (size_t index : indices) {
            kicked[index] = true;
            layerPlan_.kickedTiles.push_back(TileFallback{
                layerPlan_.renderTiles[index].targetKey,
                parentKey
            });
        }
    }

    if (parentRenderTiles.empty()) return;

    std::vector<RenderTileRef> rewritten;
    rewritten.reserve(layerPlan_.renderTiles.size());
    std::unordered_set<TileKey> emittedTargets;
    for (size_t i = 0; i < layerPlan_.renderTiles.size(); ++i) {
        if (kicked[i]) continue;
        if (emittedTargets.insert(layerPlan_.renderTiles[i].targetKey).second) {
            rewritten.push_back(layerPlan_.renderTiles[i]);
        }
    }
    for (const RenderTileRef& parentRef : parentRenderTiles) {
        if (emittedTargets.insert(parentRef.targetKey).second) {
            rewritten.push_back(parentRef);
        }
    }

    layerPlan_.kickedTileCount =
        static_cast<int>(layerPlan_.kickedTiles.size());
    layerPlan_.transitionTileCount += layerPlan_.kickedTileCount;
    layerPlan_.renderTiles = std::move(rewritten);
    recomputeRenderReadinessCounts(layerPlan_);
}

bool BasemapLayer::findRequestTileForMissingTexture(const TileKey& target,
                                                    TileKey& requestKey) const {
    std::vector<TileKey> lineage;
    TileKey candidate = target;
    while (true) {
        lineage.push_back(candidate);
        if (candidate.z <= tileScheme_->minZoom()) break;
        candidate = TilePlanBuilder::parentKey(candidate);
    }

    for (auto it = lineage.rbegin(); it != lineage.rend(); ++it) {
        if (!provider_ || !provider_->supportsTile(*it)) continue;
        if (textureCache_.contains(*it)) continue;
        requestKey = *it;
        return true;
    }
    return false;
}

std::string BasemapLayer::surfaceMeshCacheKeyForTile(
    const TileKey& key,
    const Rectangle& bounds,
    const TerrainLayer* terrainLayer,
    bool useRawQuantizedMesh) const {
    const int gridSize = surfaceGridSizeForZoom(key.z);
    const TerrainTile* terrainTile = terrainLayer
        ? terrainLayer->findBestTileForBounds(bounds)
        : nullptr;
    const bool useTerrain = terrainTile && terrainTile->valid();
    return tileCacheKey(key) + "/surface/" +
        (useTerrain
            ? std::string("terrain/") +
                  (useRawQuantizedMesh ? "raw/" : "grid/") +
                  tileCacheKey(terrainTile->key())
            : "ellipsoid") +
        "/" + std::to_string(gridSize);
}

BasemapLayer::SurfaceGpuMesh*
BasemapLayer::findSurfaceGpuMesh(const TileKey& key,
                                 const Rectangle& bounds,
                                 const TerrainLayer* terrainLayer,
                                 SurfaceMeshBuildStats* stats,
                                 bool useRawQuantizedMesh) {
    const double totalStartMs = perf::nowMs();
    const std::string ck = surfaceMeshCacheKeyForTile(
        key, bounds, terrainLayer, useRawQuantizedMesh);
    auto found = surfaceMeshCache_.find(ck);
    if (found == surfaceMeshCache_.end()) {
        return nullptr;
    }
    found->second.lastUsedFrame = layerPlan_.frameId;
    if (stats) {
        ++stats->hits;
        stats->cacheKeyMs += perf::nowMs() - totalStartMs;
        stats->totalMs += perf::nowMs() - totalStartMs;
    }
    return &found->second;
}

BasemapLayer::SurfaceGpuMesh*
BasemapLayer::getOrCreateSurfaceGpuMesh(const TileKey& key,
                                        const Rectangle& bounds,
                                        const TerrainLayer* terrainLayer,
                                        SurfaceMeshBuildStats* stats,
                                        bool useRawQuantizedMesh) {
    if (!renderDevice_) return nullptr;

    const double totalStartMs = perf::nowMs();
    const int gridSize = surfaceGridSizeForZoom(key.z);
    const double terrainLookupStartMs = perf::nowMs();
    const TerrainTile* terrainTile = terrainLayer
        ? terrainLayer->findBestTileForBounds(bounds)
        : nullptr;
    const bool useTerrain = terrainTile && terrainTile->valid();
    if (stats) stats->terrainLookupMs += perf::nowMs() - terrainLookupStartMs;
#ifdef __ANDROID__
    static int dbgCount = 0;
    if (terrainLayer && ++dbgCount <= 3) {
        __android_log_print(ANDROID_LOG_INFO, "BasemapLayer",
            "getOrCreate: z=%d useTerr=%d cacheTerr=%d",
            key.z, useTerrain, terrainLayer->cachedTileCount());
    }
#endif
    const double cacheKeyStartMs = perf::nowMs();
    const std::string ck = surfaceMeshCacheKeyForTile(
        key, bounds, terrainLayer, useRawQuantizedMesh);
    if (stats) stats->cacheKeyMs += perf::nowMs() - cacheKeyStartMs;

    auto found = surfaceMeshCache_.find(ck);
    if (found != surfaceMeshCache_.end()) {
        found->second.lastUsedFrame = layerPlan_.frameId;
        if (stats) {
            ++stats->hits;
            stats->totalMs += perf::nowMs() - totalStartMs;
        }
        return &found->second;
    }
    if (stats) ++stats->misses;

    constexpr double kTerrainSkirtHeightMeters = -100.0;

    // OpenGlobus equalizeVertices: get parent tile for edge averaging
    const TerrainTile* parentTile = nullptr;
    if (useTerrain && terrainTile) {
        TileKey parentKey = TilePlanBuilder::parentKey(terrainTile->key());
        parentTile = terrainLayer->findBestTileForKey(parentKey);
    }

    const double meshBuildStartMs = perf::nowMs();
    SurfaceTileMesh mesh = useTerrain
        ? TileSurface::buildTerrainMesh(
              bounds,
              terrainTile,
              gridSize,
              kTerrainSkirtHeightMeters,
              parentTile,
              useRawQuantizedMesh)
        : TileSurface::buildEllipsoidMesh(bounds, gridSize);
    const double meshBuildMs = perf::nowMs() - meshBuildStartMs;
    if (stats) stats->meshBuildMs += meshBuildMs;
    if (mesh.vertices.empty() || mesh.indices.empty()) return nullptr;

    const double centroidStartMs = perf::nowMs();
    const Vec3 localOrigin = meshCentroid(mesh);
    if (stats) stats->centroidMs += perf::nowMs() - centroidStartMs;

    const double vertexBuildStartMs = perf::nowMs();
    std::vector<SurfaceGpuVertex> vertices =
        makeSurfaceGpuVertices(mesh, localOrigin);
    if (stats) stats->vertexBuildMs += perf::nowMs() - vertexBuildStartMs;
    if (vertices.empty()) return nullptr;

    SurfaceGpuMesh gpuMesh;
    gpuMesh.localOriginEcef = localOrigin;
    gpuMesh.lastUsedFrame = layerPlan_.frameId;
    gpuMesh.usesTerrain = useTerrain;
    gpuMesh.usesParentTerrain = useTerrain && terrainTile->key() != key;
    gpuMesh.terrainReady = useTerrain && terrainTile->key() == key;
    gpuMesh.terrainTransition = useTerrain && terrainTile->key() != key;

    BufferDesc vbDesc;
    vbDesc.size = vertices.size() * sizeof(SurfaceGpuVertex);
    vbDesc.data = vertices.data();
    vbDesc.usage = BufferDesc::Usage::Static;
    vbDesc.type = BufferDesc::Type::Vertex;
    const double vertexBufferStartMs = perf::nowMs();
    gpuMesh.vertexBuffer = renderDevice_->createBuffer(vbDesc);
    if (stats) stats->vertexBufferMs += perf::nowMs() - vertexBufferStartMs;
    if (!gpuMesh.vertexBuffer) return nullptr;

    BufferDesc ibDesc;
    ibDesc.size = mesh.indices.size() * sizeof(uint32_t);
    ibDesc.data = mesh.indices.data();
    ibDesc.usage = BufferDesc::Usage::Static;
    ibDesc.type = BufferDesc::Type::Index;
    const double indexBufferStartMs = perf::nowMs();
    gpuMesh.indexBuffer = renderDevice_->createBuffer(ibDesc);
    if (stats) stats->indexBufferMs += perf::nowMs() - indexBufferStartMs;
    if (!gpuMesh.indexBuffer) return nullptr;

    // cesium-native alignment: per-vertex normals are used directly in the
    // shader (glTF NORMAL attribute style); no separate normal map texture
    // upload is required, eliminating per-tile RGBA8 texture allocation.

    // Water mask texture (QuantizedMesh extension ID=2)
    const double waterMaskStartMs = perf::nowMs();
    if (mesh.waterMask.valid()) {
        TextureDesc wmDesc;
        wmDesc.width = 256;
        wmDesc.height = 256;
        wmDesc.format = TextureDesc::Format::RGBA8;
        if (mesh.waterMask.allWater) {
            // Solid white = fully water
            static std::vector<uint8_t> sWhite(256 * 256 * 4, 255);
            wmDesc.data = sWhite.data();
            wmDesc.dataSize = sWhite.size();
        } else {
            wmDesc.data = mesh.waterMask.data.data();
            wmDesc.dataSize = mesh.waterMask.data.size();
        }
        wmDesc.mipmap = false;
        wmDesc.minFilter = TextureDesc::Filter::Linear;
        wmDesc.wrapS = TextureDesc::Wrap::Clamp;
        wmDesc.wrapT = TextureDesc::Wrap::Clamp;
        gpuMesh.waterMaskTexture = renderDevice_->createTexture(wmDesc);
    }
    if (stats) stats->waterMaskMs += perf::nowMs() - waterMaskStartMs;

    gpuMesh.indexCount = static_cast<int>(mesh.indices.size());
    auto [it, inserted] = surfaceMeshCache_.emplace(ck, std::move(gpuMesh));
    (void)inserted;
    const double totalMs = perf::nowMs() - totalStartMs;
    if (stats) stats->totalMs += totalMs;
    if (totalMs >= 8.0) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
            "tile=%d/%d/%d grid=%d terrain=%d parent=%d vertices=%zu indices=%zu cache=%zu mesh=%.2f",
            key.z,
            key.x,
            key.y,
            gridSize,
            useTerrain ? 1 : 0,
            (useTerrain && terrainTile && terrainTile->key() != key) ? 1 : 0,
            mesh.vertices.size(),
            mesh.indices.size(),
            surfaceMeshCache_.size(),
            meshBuildMs);
        perf::logTiming(layerPlan_.frameId,
                        "BasemapLayer.surfaceMesh.create",
                        totalMs,
                        detail);
    }
    return &it->second;
}

void BasemapLayer::evictUnusedSurfaceMeshes() {
    const double startMs = perf::nowMs();
    const size_t beforeCount = surfaceMeshCache_.size();
    if (surfaceMeshCache_.empty()) {
        pendingSurfaceMeshEvictions_.clear();
        pendingSurfaceMeshEvictionSet_.clear();
        return;
    }

    std::unordered_set<std::string> currentTargetPrefixes;
    currentTargetPrefixes.reserve(layerPlan_.renderTiles.size());
    for (const auto& renderTile : layerPlan_.renderTiles) {
        currentTargetPrefixes.insert(tileCacheKey(renderTile.targetKey) + "/surface/");
    }

    constexpr const char* kSurfaceCacheMarker = "/surface/";
    constexpr size_t kSurfaceCacheMarkerLength = 9;
    auto isCurrentRenderMesh = [&](const std::string& cacheKey) {
        const size_t markerPos = cacheKey.find(kSurfaceCacheMarker);
        if (markerPos == std::string::npos) return false;
        return currentTargetPrefixes.find(
                   cacheKey.substr(0, markerPos + kSurfaceCacheMarkerLength)) !=
            currentTargetPrefixes.end();
    };

    // Surface mesh deletion can stall the GL driver, so eviction is a memory
    // protection path rather than a steady-state movement path.
    constexpr size_t kMinSurfaceMeshCacheEntries = 1024;
    constexpr size_t kMaxSurfaceMeshCacheEntries = 1280;
    constexpr size_t kSurfaceMeshHardWatermark = 1536;
    constexpr size_t kMaxSurfaceMeshEvictionsQueuedPerRefill = 128;
    const size_t targetCapacity = std::clamp(
        layerPlan_.renderTiles.size() * 16,
        kMinSurfaceMeshCacheEntries,
        kMaxSurfaceMeshCacheEntries);
    const size_t hardWatermark = std::max(kSurfaceMeshHardWatermark,
                                          targetCapacity + 128);
    constexpr size_t kMaxSurfaceMeshDeletesPerFrame = 2;

    auto deleteQueuedMeshes = [&]() {
        size_t deleted = 0;
        while (!pendingSurfaceMeshEvictions_.empty() &&
               deleted < kMaxSurfaceMeshDeletesPerFrame &&
               surfaceMeshCache_.size() > targetCapacity) {
            std::string cacheKey = std::move(pendingSurfaceMeshEvictions_.front());
            pendingSurfaceMeshEvictions_.pop_front();
            pendingSurfaceMeshEvictionSet_.erase(cacheKey);
            if (isCurrentRenderMesh(cacheKey)) continue;
            deleted += surfaceMeshCache_.erase(cacheKey);
        }
        return deleted;
    };

    const bool hadPendingEvictions = !pendingSurfaceMeshEvictions_.empty();
    size_t deleted = deleteQueuedMeshes();

    if (cameraMoving_ && surfaceMeshCache_.size() <= hardWatermark) {
        if (deleted > 0 || hadPendingEvictions) {
            char detail[256];
            std::snprintf(detail, sizeof(detail),
                "mode=movingConsume before=%zu after=%zu deleted=%zu queued=%zu target=%zu hard=%zu current=%zu",
                beforeCount,
                surfaceMeshCache_.size(),
                deleted,
                pendingSurfaceMeshEvictions_.size(),
                targetCapacity,
                hardWatermark,
                layerPlan_.renderTiles.size());
            perf::logTimingAtLeast(layerPlan_.frameId,
                                   "BasemapLayer.evictSurfaceMeshes",
                                   perf::nowMs() - startMs,
                                   2.0,
                                   detail);
        }
        return;
    }

    if (hadPendingEvictions) {
        char detail[256];
        std::snprintf(detail, sizeof(detail),
            "mode=consume before=%zu after=%zu deleted=%zu queued=%zu target=%zu hard=%zu current=%zu",
            beforeCount,
            surfaceMeshCache_.size(),
            deleted,
            pendingSurfaceMeshEvictions_.size(),
            targetCapacity,
            hardWatermark,
            layerPlan_.renderTiles.size());
        perf::logTimingAtLeast(layerPlan_.frameId,
                               "BasemapLayer.evictSurfaceMeshes",
                               perf::nowMs() - startMs,
                               2.0,
                               detail);
        return;
    }

    if (surfaceMeshCache_.size() <= hardWatermark) {
        if (deleted > 0) {
            char detail[256];
            std::snprintf(detail, sizeof(detail),
                "mode=belowHard before=%zu after=%zu deleted=%zu queued=%zu target=%zu hard=%zu current=%zu",
                beforeCount,
                surfaceMeshCache_.size(),
                deleted,
                pendingSurfaceMeshEvictions_.size(),
                targetCapacity,
                hardWatermark,
                layerPlan_.renderTiles.size());
            perf::logTimingAtLeast(layerPlan_.frameId,
                                   "BasemapLayer.evictSurfaceMeshes",
                                   perf::nowMs() - startMs,
                                   2.0,
                                   detail);
        }
        return;
    }

    std::vector<std::pair<uint64_t, const std::string*>> evictionCandidates;
    evictionCandidates.reserve(surfaceMeshCache_.size());
    for (const auto& entry : surfaceMeshCache_) {
        if (isCurrentRenderMesh(entry.first)) continue;
        if (pendingSurfaceMeshEvictionSet_.find(entry.first) !=
            pendingSurfaceMeshEvictionSet_.end()) {
            continue;
        }
        evictionCandidates.emplace_back(entry.second.lastUsedFrame, &entry.first);
    }

    const size_t targetQueueCount = std::min(
        surfaceMeshCache_.size() > targetCapacity
            ? surfaceMeshCache_.size() - targetCapacity
            : size_t{0},
        kMaxSurfaceMeshEvictionsQueuedPerRefill);
    if (targetQueueCount < evictionCandidates.size()) {
        auto nth = evictionCandidates.begin() +
            static_cast<std::ptrdiff_t>(targetQueueCount);
        std::nth_element(evictionCandidates.begin(), nth, evictionCandidates.end(),
            [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return *a.second < *b.second;
            });
        evictionCandidates.resize(targetQueueCount);
    }

    size_t queued = 0;
    for (const auto& candidate : evictionCandidates) {
        if (queued >= targetQueueCount) {
            break;
        }
        pendingSurfaceMeshEvictions_.push_back(*candidate.second);
        pendingSurfaceMeshEvictionSet_.insert(*candidate.second);
        ++queued;
    }

    char detail[256];
    std::snprintf(detail, sizeof(detail),
        "mode=refill before=%zu after=%zu candidates=%zu queuedNew=%zu queued=%zu deleted=%zu target=%zu hard=%zu current=%zu",
        beforeCount,
        surfaceMeshCache_.size(),
        evictionCandidates.size(),
        queued,
        pendingSurfaceMeshEvictions_.size(),
        deleted,
        targetCapacity,
        hardWatermark,
        layerPlan_.renderTiles.size());
    perf::logTimingAtLeast(layerPlan_.frameId,
                           "BasemapLayer.evictSurfaceMeshes",
                           perf::nowMs() - startMs,
                           2.0,
                           detail);
}

size_t BasemapLayer::surfaceMeshBytes() const {
    size_t total = 0;
    for (const auto& entry : surfaceMeshCache_) {
        const int indexCount = entry.second.indexCount;
        const int gridSize = indexCount > 0
            ? static_cast<int>(std::sqrt(static_cast<double>(indexCount) / 6.0))
            : 0;
        const size_t vertexCount = static_cast<size_t>((gridSize + 1) * (gridSize + 1));
        total += vertexCount * sizeof(SurfaceGpuVertex) +
                 static_cast<size_t>(indexCount) * sizeof(uint32_t);
    }
    return total;
}

int BasemapLayer::exactAttachmentCount() const {
    int count = 0;
    for (const auto& renderTile : layerPlan_.renderTiles) {
        if (renderTile.source == TileRenderSource::Exact) {
            ++count;
        }
    }
    return count;
}

int BasemapLayer::parentFallbackAttachmentCount() const {
    int count = 0;
    for (const auto& renderTile : layerPlan_.renderTiles) {
        if (renderTile.source == TileRenderSource::ParentFallback) {
            ++count;
        }
    }
    return count;
}

int BasemapLayer::minRenderTargetZoom() const {
    if (layerPlan_.renderTiles.empty()) return 0;
    int minZoom = layerPlan_.renderTiles.front().targetKey.z;
    for (const auto& renderTile : layerPlan_.renderTiles) {
        minZoom = std::min(minZoom, renderTile.targetKey.z);
    }
    return minZoom;
}

int BasemapLayer::maxRenderTargetZoom() const {
    if (layerPlan_.renderTiles.empty()) return 0;
    int maxZoom = layerPlan_.renderTiles.front().targetKey.z;
    for (const auto& renderTile : layerPlan_.renderTiles) {
        maxZoom = std::max(maxZoom, renderTile.targetKey.z);
    }
    return maxZoom;
}

int BasemapLayer::minRenderTextureZoom() const {
    if (layerPlan_.renderTiles.empty()) return 0;
    int minZoom = layerPlan_.renderTiles.front().textureKey.z;
    for (const auto& renderTile : layerPlan_.renderTiles) {
        minZoom = std::min(minZoom, renderTile.textureKey.z);
    }
    return minZoom;
}

int BasemapLayer::maxRenderTextureZoom() const {
    if (layerPlan_.renderTiles.empty()) return 0;
    int maxZoom = layerPlan_.renderTiles.front().textureKey.z;
    for (const auto& renderTile : layerPlan_.renderTiles) {
        maxZoom = std::max(maxZoom, renderTile.textureKey.z);
    }
    return maxZoom;
}

int BasemapLayer::terrainSurfaceMeshCount() const {
    int count = 0;
    for (const auto& entry : surfaceMeshCache_) {
        if (entry.second.usesTerrain) {
            ++count;
        }
    }
    return count;
}

int BasemapLayer::terrainParentFallbackMeshCount() const {
    int count = 0;
    for (const auto& entry : surfaceMeshCache_) {
        if (entry.second.usesParentTerrain) {
            ++count;
        }
    }
    return count;
}

int BasemapLayer::ellipsoidSurfaceMeshCount() const {
    int count = 0;
    for (const auto& entry : surfaceMeshCache_) {
        if (!entry.second.usesTerrain) {
            ++count;
        }
    }
    return count;
}

int BasemapLayer::terrainReadySurfaceMeshCount() const {
    int count = 0;
    for (const auto& entry : surfaceMeshCache_) {
        if (entry.second.terrainReady) {
            ++count;
        }
    }
    return count;
}

int BasemapLayer::terrainTransitionSurfaceMeshCount() const {
    int count = 0;
    for (const auto& entry : surfaceMeshCache_) {
        if (entry.second.terrainTransition) {
            ++count;
        }
    }
    return count;
}

std::string BasemapLayer::tileCacheKey(const TileKey& key) const {
    return id_ + "/" + provider_->id() + "/" + key.schemeId + "/" +
           std::to_string(key.z) + "/" +
           std::to_string(key.x) + "/" +
           std::to_string(key.y);
}

BasemapLayer::ImageryAtlasLookup BasemapLayer::findImageryAtlasEntry(
    const TileKey& key) {
    const std::string cacheKey = tileCacheKey(key);
    for (auto& [tileSize, atlas] : imageryAtlases_) {
        (void)tileSize;
        auto it = atlas.entries.find(cacheKey);
        if (it != atlas.entries.end() && atlas.texture) {
            it->second.lastUsedFrame = layerPlan_.frameId;
            return ImageryAtlasLookup{atlas.texture.get(), it->second};
        }
    }
    return {};
}

void BasemapLayer::resetImageryAtlas(int tileSize) {
    auto it = imageryAtlases_.find(tileSize);
    if (it == imageryAtlases_.end()) return;
    ImageryAtlas& atlas = it->second;
    atlas.texture.reset();
    atlas.atlasSize = 0;
    atlas.tileSize = 0;
    atlas.columns = 0;
    atlas.nextSlot = 0;
    atlas.replacements = 0;
    atlas.entries.clear();
    atlas.slotKeys.clear();
    ++atlas.generation;
}

bool BasemapLayer::uploadImageryAtlasTile(const TileKey& key, const DecodedImage& image) {
    if (!renderDevice_ || image.width <= 0 || image.height <= 0) {
        return false;
    }
    if (image.width != image.height || image.channels < 3) {
        return false;
    }

    constexpr int kPreferredAtlasSize = 4096;
    const int deviceMaxTextureSize = renderDevice_->maxTextureSize();
    const int atlasSize = std::min(kPreferredAtlasSize, deviceMaxTextureSize);
    if (atlasSize < image.width) {
        return false;
    }

    ImageryAtlas& atlas = imageryAtlases_[image.width];
    if (!atlas.texture ||
        atlas.atlasSize != atlasSize ||
        atlas.tileSize != image.width) {
        resetImageryAtlas(image.width);
        TextureDesc atlasDesc;
        atlasDesc.width = atlasSize;
        atlasDesc.height = atlasSize;
        atlasDesc.format = TextureDesc::Format::RGBA8;
        atlasDesc.data = nullptr;
        atlasDesc.dataSize = 0;
        atlasDesc.mipmap = false;
        atlasDesc.minFilter = TextureDesc::Filter::Linear;
        atlasDesc.magFilter = TextureDesc::Filter::Linear;
        atlasDesc.wrapS = TextureDesc::Wrap::Clamp;
        atlasDesc.wrapT = TextureDesc::Wrap::Clamp;
        atlas.texture = renderDevice_->createTexture(atlasDesc);
        if (!atlas.texture) {
            resetImageryAtlas(image.width);
            return false;
        }
        atlas.atlasSize = atlasSize;
        atlas.tileSize = image.width;
        atlas.columns = std::max(1, atlasSize / image.width);
        atlas.slotKeys.assign(
            static_cast<size_t>(atlas.columns * atlas.columns),
            std::string());
    }

    const int capacity = atlas.columns * atlas.columns;
    const std::string cacheKey = tileCacheKey(key);
    int slot = -1;
    auto existing = atlas.entries.find(cacheKey);
    if (existing != atlas.entries.end()) {
        slot = existing->second.slot;
    } else if (atlas.nextSlot < capacity) {
        slot = atlas.nextSlot++;
    } else {
        std::unordered_set<std::string> protectedKeys;
        protectedKeys.reserve(layerPlan_.renderTiles.size());
        for (const RenderTileRef& renderTile : layerPlan_.renderTiles) {
            protectedKeys.insert(tileCacheKey(renderTile.textureKey));
        }
        uint64_t oldestFrame = std::numeric_limits<uint64_t>::max();
        std::string evictKey;
        for (const auto& entry : atlas.entries) {
            if (entry.second.slot < 0) continue;
            if (protectedKeys.find(entry.first) != protectedKeys.end()) continue;
            if (entry.second.lastUsedFrame < oldestFrame) {
                oldestFrame = entry.second.lastUsedFrame;
                evictKey = entry.first;
                slot = entry.second.slot;
            }
        }
        if (slot < 0 || evictKey.empty()) {
            return false;
        }
        atlas.entries.erase(evictKey);
        ++atlas.replacements;
    }

    std::vector<uint8_t> convertedRgba;
    const uint8_t* uploadPixels = image.pixels.data();
    if (image.channels != 4) {
        convertedRgba.resize(static_cast<size_t>(image.width) *
                             static_cast<size_t>(image.height) * 4u);
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const size_t src = (static_cast<size_t>(y) * image.width + x) *
                                   static_cast<size_t>(image.channels);
                const size_t dst = (static_cast<size_t>(y) * image.width + x) * 4u;
                convertedRgba[dst + 0] = image.pixels[src + 0];
                convertedRgba[dst + 1] = image.pixels[src + 1];
                convertedRgba[dst + 2] = image.pixels[src + 2];
                convertedRgba[dst + 3] = 255;
            }
        }
        uploadPixels = convertedRgba.data();
    }

    const int col = slot % atlas.columns;
    const int row = slot / atlas.columns;
    const int x = col * atlas.tileSize;
    const int y = row * atlas.tileSize;
    const bool uploaded = renderDevice_->updateTextureRegion(
        atlas.texture.get(),
        x,
        y,
        atlas.tileSize,
        atlas.tileSize,
        uploadPixels,
        static_cast<size_t>(atlas.tileSize) * 4u);
    if (!uploaded) {
        return false;
    }

    const float atlasScale =
        static_cast<float>(atlas.tileSize) /
        static_cast<float>(atlas.atlasSize);
    ImageryAtlasEntry entry;
    entry.offsetU = static_cast<float>(x) / static_cast<float>(atlas.atlasSize);
    entry.offsetV = static_cast<float>(y) / static_cast<float>(atlas.atlasSize);
    entry.scaleU = atlasScale;
    entry.scaleV = atlasScale;
    entry.slot = slot;
    entry.lastUsedFrame = layerPlan_.frameId;
    atlas.entries[cacheKey] = entry;
    if (slot >= 0 && slot < static_cast<int>(atlas.slotKeys.size())) {
        atlas.slotKeys[static_cast<size_t>(slot)] = cacheKey;
    }
    return true;
}

void BasemapLayer::buildRenderCommands(Renderer& renderer,
                                        const TerrainLayer* terrainLayer,
                                        RenderCommandList& commands) {
    std::vector<BasemapLayer*> overlayLayers;
    buildRenderCommands(renderer, terrainLayer, overlayLayers, commands);
}

bool BasemapLayer::resolveAttachmentForRenderTile(const RenderTileRef& renderTile,
                                                  ImageryAttachment& out) {
    if (!visible_) return false;

    const TileKey& key = renderTile.targetKey;
    TileKey textureKey = renderTile.textureKey;
    Texture* tex = textureCache_.get(textureKey);
    ImageryFallbackSource fallbackSource =
        renderTile.source == TileRenderSource::Exact
            ? ImageryFallbackSource::Exact
            : ImageryFallbackSource::Parent;
    if (!tex) {
        tex = findFallbackTexture(key, textureKey);
        fallbackSource = ImageryFallbackSource::Parent;
    }
    if (!tex) return false;

    Rectangle bounds = tileScheme_->tileToRectangle(key);
    Rectangle textureBounds = tileScheme_->tileToRectangle(textureKey);
    TileTextureWindow uv = TileSurface::computeTranslationAndScale(bounds, textureBounds);
    if (tex->width() > 0 && tex->height() > 0) {
        const float insetU = 0.5f / static_cast<float>(tex->width());
        const float insetV = 0.5f / static_cast<float>(tex->height());
        uv.offsetU += insetU;
        uv.offsetV += insetV;
        uv.scaleU = std::max(0.0f, uv.scaleU - insetU * 2.0f);
        uv.scaleV = std::max(0.0f, uv.scaleV - insetV * 2.0f);
    }

    out = ImageryAttachment{
        id_,
        provider_ ? provider_->id() : "",
        textureKey,
        tex,
        uv.offsetU,
        uv.offsetV,
        uv.scaleU,
        uv.scaleV,
        opacity_,
        fallbackSource
    };
    return true;
}

bool BasemapLayer::resolveAttachmentForBounds(const Rectangle& bounds,
                                              int preferredZoom,
                                              ImageryAttachment& out) {
    if (!visible_) return false;

    const double centerLng = rectangleCenterLongitude(bounds);
    const double centerLat = bounds.south() + bounds.height() * 0.5;
    const int startZoom = std::min(tileScheme_->maxZoom(), preferredZoom);
    for (int z = startZoom; z >= tileScheme_->minZoom(); --z) {
        TileKey textureKey = tileScheme_->positionToTile(centerLng, centerLat, z);
        Rectangle textureBounds = tileScheme_->tileToRectangle(textureKey);
        if (!textureBounds.contains(bounds)) {
            continue;
        }
        Texture* tex = textureCache_.get(textureKey);
        if (!tex) {
            continue;
        }

        TileTextureWindow uv = TileSurface::computeTranslationAndScale(bounds, textureBounds);
        if (tex->width() > 0 && tex->height() > 0) {
            const float insetU = 0.5f / static_cast<float>(tex->width());
            const float insetV = 0.5f / static_cast<float>(tex->height());
            uv.offsetU += insetU;
            uv.offsetV += insetV;
            uv.scaleU = std::max(0.0f, uv.scaleU - insetU * 2.0f);
            uv.scaleV = std::max(0.0f, uv.scaleV - insetV * 2.0f);
        }

        out = ImageryAttachment{
            id_,
            provider_ ? provider_->id() : "",
            textureKey,
            tex,
            uv.offsetU,
            uv.offsetV,
            uv.scaleU,
            uv.scaleV,
            opacity_,
            z == preferredZoom
                ? ImageryFallbackSource::Exact
                : ImageryFallbackSource::Parent
        };
        return true;
    }

    return false;
}

bool BasemapLayer::buildTerrainPrimaryRenderCommands(Renderer& renderer,
                                                     const TerrainLayer* terrainLayer,
                                                     const std::vector<BasemapLayer*>& overlayLayers,
                                                     RenderCommandList& commands) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "TerrainPrimary",
        "ENTER terrain=%p vis=%d rd=%p inst=%d",
        terrainLayer, terrainLayer ? terrainLayer->visible() : 0,
        renderDevice_, renderDevice_ ? renderDevice_->supportsInstancing() : 0);
#endif
    if (!terrainLayer || !terrainLayer->visible() || !visible_) return false;
    if (!renderDevice_ || !renderDevice_->supportsInstancing()) return false;

    const std::vector<const TerrainTile*> terrainTiles =
        terrainLayer->visibleLoadedTiles();
#ifdef __ANDROID__
    static int tplog = 0;
    if (++tplog <= 5) {
        __android_log_print(ANDROID_LOG_INFO, "TerrainPrimary",
            "visibleLoaded=%zu cached=%d visible=%zu",
            terrainTiles.size(),
            terrainLayer->cachedTileCount(),
            terrainLayer->visibleTiles().size());
    }
#endif
    if (terrainTiles.empty()) return false;

    constexpr int kGridSize = 64;
    const int n = kGridSize + 1;
    const auto& ellipsoid = Ellipsoid::WGS84();

    int emitted = 0;
    constexpr int kMaxTerrainDraws = 48;

    for (const TerrainTile* terrainTile : terrainTiles) {
        if (!terrainTile || !terrainTile->valid()) continue;
        if (emitted >= kMaxTerrainDraws) break;

        const Rectangle& bounds = terrainTile->bounds();
        ImageryAttachment attachment;
        if (!resolveAttachmentForBounds(bounds, terrainTile->key().z, attachment)) {
            if (!placeholderTexture_ && renderDevice_) {
                uint8_t white[4] = {255, 255, 255, 255};
                TextureDesc desc;
                desc.width = 1; desc.height = 1;
                desc.format = TextureDesc::Format::RGBA8;
                desc.data = white;
                desc.minFilter = TextureDesc::Filter::Nearest;
                desc.magFilter = TextureDesc::Filter::Nearest;
                placeholderTexture_ = renderDevice_->createTexture(desc);
            }
            if (!placeholderTexture_) continue;
            attachment.texture = placeholderTexture_.get();
            attachment.uvOffsetU = 0.0f;
            attachment.uvOffsetV = 0.0f;
            attachment.uvScaleU = 1.0f;
            attachment.uvScaleV = 1.0f;
            attachment.opacity = 1.0f;
        }

        // cesium-native: use raw QM triangulation directly.
        // parseToSurfaceTileMesh preserves the optimized mesh topology,
        // oct-encoded normals, and proper edge skirts.
        const auto& rawData = terrainTile->heightmap()->rawData;
        std::unique_ptr<SurfaceTileMesh> qmMesh;
        if (!rawData.empty()) {
            qmMesh = QuantizedMeshParser::parseToSurfaceTileMesh(
                rawData.data(), rawData.size(), bounds);
        }
        if (!qmMesh) {
            // Fallback: regular grid with height sampling
            struct TerrainGpuVertex { float pos[3]; float uv[2]; };
            std::vector<TerrainGpuVertex> verts(n * n);
            for (int y = 0; y < n; ++y) {
                double v = static_cast<double>(y) / static_cast<double>(kGridSize);
                for (int x = 0; x < n; ++x) {
                    double u = static_cast<double>(x) / static_cast<double>(kGridSize);
                    TileSurfaceVertex sv = TileSurface::vertexForUnitUv(bounds, u, v);
                    Cartographic cart = ellipsoid.cartesianToCartographic(sv.ecef);
                    double h = static_cast<double>(terrainTile->sampleHeight(
                        cart.longitude(), cart.latitude()));
                    Cartographic tc = Cartographic::fromRadians(
                        cart.longitude(), cart.latitude(), h);
                    Vec3 ecef = ellipsoid.cartographicToCartesian(tc);
                    verts[y * n + x].pos[0] = static_cast<float>(ecef.x());
                    verts[y * n + x].pos[1] = static_cast<float>(ecef.y());
                    verts[y * n + x].pos[2] = static_cast<float>(ecef.z());
                    verts[y * n + x].uv[0] = static_cast<float>(u);
                    verts[y * n + x].uv[1] = static_cast<float>(v);
                }
            }
            BufferDesc vbDesc;
            vbDesc.size = verts.size() * sizeof(TerrainGpuVertex);
            vbDesc.data = verts.data();
            vbDesc.usage = BufferDesc::Usage::Static;
            vbDesc.type = BufferDesc::Type::Vertex;
            auto vbo = renderDevice_->createBuffer(vbDesc);
            if (!vbo) continue;
            auto cmd = renderer.makeSurfaceTileCommand(
                attachment.texture, vbo.get(), nullptr, 0);
            cmd.surfaceTileUv = {attachment.uvOffsetU, attachment.uvOffsetV,
                                 attachment.uvScaleU, attachment.uvScaleV};
            cmd.surfaceTileOpacity = attachment.opacity;
            cmd.surfaceTransitionOpacity = 1.0f;
            cmd.surfaceGeneration = static_cast<float>(generation_);
            cmd.frameId = layerPlan_.frameId;
            cmd.generation = generation_;
            commands.push_back(std::move(cmd));
            frameBuffers_.push_back(std::move(vbo));
            ++emitted;
            continue;
        }

        // QM mesh: build VBO/IBO from irregular triangulation
        const Vec3 localOrigin = meshCentroid(*qmMesh);
        std::vector<SurfaceGpuVertex> gpuVerts = makeSurfaceGpuVertices(*qmMesh, localOrigin);

        BufferDesc vbDesc;
        vbDesc.size = gpuVerts.size() * sizeof(SurfaceGpuVertex);
        vbDesc.data = gpuVerts.data();
        vbDesc.usage = BufferDesc::Usage::Static;
        vbDesc.type = BufferDesc::Type::Vertex;
        auto vbo = renderDevice_->createBuffer(vbDesc);
        if (!vbo) continue;

        BufferDesc ibDesc;
        ibDesc.size = qmMesh->indices.size() * sizeof(uint32_t);
        ibDesc.data = qmMesh->indices.data();
        ibDesc.usage = BufferDesc::Usage::Static;
        ibDesc.type = BufferDesc::Type::Index;
        auto ibo = renderDevice_->createBuffer(ibDesc);
        if (!ibo) continue;

        // QM mesh: per-tile VBO + per-tile IBO, unified shader
        auto cmd = renderer.makeSurfaceTileCommand(
            attachment.texture, vbo.get(), ibo.get(),
            static_cast<int>(qmMesh->indices.size()));
        cmd.surfaceTileOrigin = {
            static_cast<float>(localOrigin.x()),
            static_cast<float>(localOrigin.y()),
            static_cast<float>(localOrigin.z())
        };
        if (attachment.texture) cmd.textures.push_back(attachment.texture);
        cmd.surfaceTileUv = {attachment.uvOffsetU, attachment.uvOffsetV,
                             attachment.uvScaleU, attachment.uvScaleV};
        cmd.surfaceTileOpacity = attachment.opacity;
        cmd.surfaceTransitionOpacity = 1.0f;
        cmd.surfaceGeneration = static_cast<float>(generation_);
        cmd.frameId = layerPlan_.frameId;
        cmd.generation = generation_;
        commands.push_back(std::move(cmd));

        // Keep buffers alive for this frame
        frameBuffers_.push_back(std::move(vbo));
        frameBuffers_.push_back(std::move(ibo));
        ++emitted;
    }

    return emitted > 0;
}

void BasemapLayer::buildRenderCommands(Renderer& renderer,
                                        const TerrainLayer* terrainLayer,
                                        const std::vector<BasemapLayer*>& overlayLayers,
                                        RenderCommandList& commands) {
    if (!visible_) return;
    constexpr bool kTerrainPrimaryProbeEnabled = true;

    const double totalStartMs = perf::nowMs();
    double textureLookupMs = 0.0;
    double boundsMs = 0.0;
    double uvMs = 0.0;
    double commandMs = 0.0;
    double instanceBufferMs = 0.0;
    size_t instanceBufferBytes = 0;
    int instanceBufferCreates = 0;
    int instanceBufferUpdates = 0;
    int instancedTileCount = 0;
    int instancedExactTextures = 0;
    int instancedParentTextures = 0;
    int instancedAtlasTiles = 0;
    int blendedTileCount = 0;
    int blendedExactTileCount = 0;
    int blendedParentTileCount = 0;
    int rawBlendedTileCount = 0;
    float minEffectiveOpacity = 1.0f;
    float rawMinEffectiveOpacity = 1.0f;
    size_t maxInstancesPerTexture = 0;
    SurfaceMeshBuildStats meshStats;
    int visited = 0;
    int missingTexture = 0;
    int emitted = 0;
    int meshBuildBudget = cameraMoving_ ? 1 : 2;
    const double meshBuildFrameBudgetMs = cameraMoving_ ? 2.0 : 6.0;
    int meshBuildDeferred = 0;
    int meshParentFallback = 0;
    int meshParentCovered = 0;
    const size_t startCommands = commands.size();
    std::unordered_set<TileKey> emittedMeshFallbackTargets;
    struct RenderOrderEntry {
        const RenderTileRef* renderTile = nullptr;
        double priority = 0.0;
    };
    std::vector<RenderOrderEntry> renderOrder;
    renderOrder.reserve(layerPlan_.renderTiles.size());
    for (const RenderTileRef& renderTile : layerPlan_.renderTiles) {
        renderOrder.push_back(RenderOrderEntry{
            &renderTile,
            viewImportancePriority(
                *tileScheme_,
                renderTile.targetKey,
                lastCameraPosition_,
                lastCameraDirection_,
                hasInteractionFocus_,
                interactionFocusDirection_)
        });
    }
    std::stable_sort(renderOrder.begin(), renderOrder.end(),
        [](const RenderOrderEntry& lhs, const RenderOrderEntry& rhs) {
            return lhs.priority < rhs.priority;
        });

    for (const RenderOrderEntry& renderEntry : renderOrder) {
        const RenderTileRef& renderTile = *renderEntry.renderTile;
        ++visited;
        const TileKey& key = renderTile.targetKey;
        const TileKey& textureKey = renderTile.textureKey;
        const double textureLookupStartMs = perf::nowMs();
        Texture* tex = textureCache_.get(textureKey);
        textureLookupMs += perf::nowMs() - textureLookupStartMs;

        if (!tex) {
            ++missingTexture;
            continue;
        }

        const double boundsStartMs = perf::nowMs();
        Rectangle bounds = tileScheme_->tileToRectangle(key);
        Rectangle textureBounds = tileScheme_->tileToRectangle(textureKey);
        boundsMs += perf::nowMs() - boundsStartMs;
        const double uvStartMs = perf::nowMs();
        TileTextureWindow uv = TileSurface::computeTranslationAndScale(bounds, textureBounds);
        if (tex->width() > 0 && tex->height() > 0) {
            const float insetU = 0.5f / static_cast<float>(tex->width());
            const float insetV = 0.5f / static_cast<float>(tex->height());
            uv.offsetU += insetU;
            uv.offsetV += insetV;
            uv.scaleU = std::max(0.0f, uv.scaleU - insetU * 2.0f);
            uv.scaleV = std::max(0.0f, uv.scaleV - insetV * 2.0f);
        }
        uvMs += perf::nowMs() - uvStartMs;

        ImageryAttachment attachment{
            id_,
            provider_ ? provider_->id() : "",
            textureKey,
            tex,
            uv.offsetU,
            uv.offsetV,
            uv.scaleU,
            uv.scaleV,
            opacity_,
            renderTile.source == TileRenderSource::Exact
                ? ImageryFallbackSource::Exact
                : ImageryFallbackSource::Parent
        };

        ImageryAttachment commandAttachment = attachment;
        Rectangle commandBounds = bounds;

        // cesium-native: per-tile VBO + draw call (no instancing)
        {
            const double commandStartMs = perf::nowMs();
            const Vec3 localOrigin = ellipsoidSurfaceOriginForBounds(commandBounds);

            // Build/cache ellipsoid mesh VBO per tile key
            std::string vboKey = tileCacheKey(key) + "/vbo";
            vboCacheUsedKeys_.insert(vboKey);
            Buffer* vbo = nullptr;
            auto vboIt = surfaceVboCache_.find(vboKey);
            if (vboIt != surfaceVboCache_.end()) {
                vbo = vboIt->second.get();
            } else if (renderDevice_) {
                SurfaceTileMesh mesh = TileSurface::buildEllipsoidMesh(commandBounds, 64);
                auto gpuVerts = makeSurfaceGpuVertices(mesh, localOrigin);
                BufferDesc vbDesc;
                vbDesc.size = gpuVerts.size() * sizeof(SurfaceGpuVertex);
                vbDesc.data = gpuVerts.data();
                vbDesc.usage = BufferDesc::Usage::Static;
                vbDesc.type = BufferDesc::Type::Vertex;
                auto newVbo = renderDevice_->createBuffer(vbDesc);
                if (newVbo) {
                    vbo = newVbo.get();
                    surfaceVboCache_[vboKey] = std::move(newVbo);
                }
            }
            if (!vbo) continue;

            const float rawTransitionOpacity =
                std::clamp(renderTile.transitionOpacity, 0.0f, 1.0f);
            const float renderTransitionOpacity =
                transitionOpacityForSurfaceDraw(
                    commandAttachment, rawTransitionOpacity, cameraMoving_);

            auto cmd = renderer.makeSurfaceTileCommand(
                commandAttachment.texture, vbo, nullptr, 0);
            cmd.surfaceTileUv = {commandAttachment.uvOffsetU, commandAttachment.uvOffsetV,
                                 commandAttachment.uvScaleU, commandAttachment.uvScaleV};
            cmd.surfaceTileOrigin = {
                static_cast<float>(localOrigin.x()),
                static_cast<float>(localOrigin.y()),
                static_cast<float>(localOrigin.z())
            };
            cmd.surfaceTileOpacity = commandAttachment.opacity;
            cmd.surfaceTransitionOpacity = renderTransitionOpacity;
            cmd.surfaceGeneration = static_cast<float>(generation_);
            cmd.frameId = layerPlan_.frameId;
            cmd.generation = generation_;

            const float effectiveOpacity =
                commandAttachment.opacity * renderTransitionOpacity;
            if (effectiveOpacity < 0.999f) {
                cmd.blend = true;
                ++blendedTileCount;
                minEffectiveOpacity = std::min(minEffectiveOpacity, effectiveOpacity);
                if (commandAttachment.fallbackSource == ImageryFallbackSource::Exact) {
                    ++blendedExactTileCount;
                } else {
                    ++blendedParentTileCount;
                }
            }
            commands.push_back(std::move(cmd));
            ++emitted;
            commandMs += perf::nowMs() - commandStartMs;
        }
    }

    // Per-tile VBO cache cleanup: evict entries not used this frame
    for (auto it = surfaceVboCache_.begin(); it != surfaceVboCache_.end();) {
        if (vboCacheUsedKeys_.find(it->first) == vboCacheUsedKeys_.end()) {
            it = surfaceVboCache_.erase(it);
        } else {
            ++it;
        }
    }
    vboCacheUsedKeys_.clear();

    int terrainPrimaryCommandCount = 0;
    {
        const size_t beforeTerrainPrimary = commands.size();
        if (buildTerrainPrimaryRenderCommands(
                renderer, terrainLayer, overlayLayers, commands)) {
            terrainPrimaryCommandCount =
                static_cast<int>(commands.size() - beforeTerrainPrimary);
        }
    }

    const bool gapProbeActive =
        cameraMoving_ &&
        (layerPlan_.missingTileCount > 0 ||
         missingTexture > 0 ||
         meshBuildDeferred > 0 ||
         meshParentCovered > 0 ||
         blendedTileCount > 0 ||
         rawBlendedTileCount > 0);
    if (!gapProbeActive) {
        minEffectiveOpacity = 1.0f;
        rawMinEffectiveOpacity = 1.0f;
    }

    char detail[1024];
    std::snprintf(detail, sizeof(detail),
        "visited=%d emitted=%d terrainPrimary=%d missingTex=%d moving=%d gapProbe=%d desired=%zu renderRefs=%zu "
        "blended=%d minOpacity=%.3f qRender=%d qWalk=%d qNot=%d vboCache=%zu cmdDelta=%zu tex=%.2f cmd=%.2f",
        visited, emitted, terrainPrimaryCommandCount, missingTexture,
        cameraMoving_ ? 1 : 0, gapProbeActive ? 1 : 0,
        layerPlan_.desiredTiles.size(), layerPlan_.renderTiles.size(),
        blendedTileCount, minEffectiveOpacity,
        layerPlan_.quadtreeRenderingNodeCount,
        layerPlan_.quadtreeWalkthroughNodeCount,
        layerPlan_.quadtreeNotRenderingNodeCount,
        surfaceVboCache_.size(),
        commands.size() - startCommands,
        textureLookupMs, commandMs);
    perf::logTiming(layerPlan_.frameId,
                    "BasemapLayer.buildRenderCommands",
                    perf::nowMs() - totalStartMs,
                    detail);

#ifdef __ANDROID__
    if (gapProbeActive) {
        __android_log_print(ANDROID_LOG_INFO, "BasemapGapProbe",
            "frame=%llu layer=%s emitted=%d missingTex=%d blended=%d",
            static_cast<unsigned long long>(layerPlan_.frameId),
            id_.c_str(), emitted, missingTexture, blendedTileCount);
    }
#endif
}

} // namespace earth_engine
