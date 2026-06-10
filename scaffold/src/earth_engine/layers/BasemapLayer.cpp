#include "BasemapLayer.h"
#ifdef __ANDROID__
#include <android/log.h>
#endif
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../core/math/Rectangle.h"
#include "../tiling/SurfaceTile.h"
#include "../tiling/TileSurface.h"
#include "TerrainLayer.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace earth_engine {
namespace {

struct SurfaceGpuVertex {
    float position[3];
    // normal removed — computed in vertex shader from ECEF position
    float texcoord[2];
};

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

bool isDescendantOf(TileKey candidate, const TileKey& ancestor) {
    if (candidate.schemeId != ancestor.schemeId || candidate.z <= ancestor.z) {
        return false;
    }
    while (candidate.z > ancestor.z) {
        candidate = TilePlanBuilder::parentKey(candidate);
    }
    return candidate == ancestor;
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

    applyPlan(plan, frameState.camera->position());
    loadMissingTiles();
}

void BasemapLayer::applyPlan(const TilePlan& plan, const Vec3& cameraPosition) {
    if (!visible_) return;

    lastCameraPosition_ = cameraPosition;
    if (plan.zoom != tilePlan_.zoom ||
        plan.visibleTiles != tilePlan_.visibleTiles) {
        ++generation_;
    }
    tilePlan_ = plan;
    rebuildLayerPlan();
    processPendingUploads();
}

void BasemapLayer::loadMissingTiles() {
    constexpr double kRetryBackoffSec = 2.0;
    constexpr int kMaxRetries = 3;
    constexpr size_t kOpenGlobusLoadingBatchSize = 12;

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

    for (const auto& key : layerPlan_.requestTiles) {
        if (requestedGeneration_.size() >= kOpenGlobusLoadingBatchSize) {
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
    }
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
    std::deque<PendingUpload> batch;
    {
        std::lock_guard<std::mutex> lock(pendingQueue_->mutex);
        constexpr size_t kMaxUploadsPerFrame = 2;
        while (!pendingQueue_->queue.empty() && batch.size() < kMaxUploadsPerFrame) {
            batch.push_back(std::move(pendingQueue_->queue.front()));
            pendingQueue_->queue.pop_front();
        }
    }

    for (auto& item : batch) {
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
            continue;
        }
        if (!isCurrentPlanTileOrAncestor(item.key)) {
            clearRequestIfCurrent();
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
        texDesc.wrapS = TextureDesc::Wrap::Clamp;
        texDesc.wrapT = TextureDesc::Wrap::Clamp;

        auto texture = renderDevice_->createTexture(texDesc);
        if (texture) {
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
        }
    }
    rebuildLayerPlan();
}

void BasemapLayer::rebuildLayerPlan() {
    const LayerTilePlan previousPlan = layerPlan_;
    layerPlan_ = LayerTilePlan{};
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
    std::unordered_set<TileKey> requestSet;

    std::unordered_set<TileKey> desiredSet;
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

    for (const auto& key : layerPlan_.desiredTiles) {
        const float lodTransitionOpacity = transitionOpacityForTile(tilePlan_, key);
        if (textureCache_.contains(key)) {
            layerPlan_.renderTiles.push_back(RenderTileRef{
                key,
                key,
                TileRenderSource::Exact,
                TileReadinessState::Ready,
                lodTransitionOpacity});
            ++layerPlan_.readyTileCount;
            continue;
        }

        TileKey fallbackKey = key;
        const bool hasFallback = findFallbackTexture(key, fallbackKey) != nullptr;
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

        TileKey requestKey = key;
        if (findRequestTileForMissingTexture(key, requestKey) &&
            requestSet.insert(requestKey).second) {
            layerPlan_.requestTiles.push_back(requestKey);
        }
        if (!hasFallback) {
            ++layerPlan_.missingTileCount;
        }
    }
    applyAncestorMeetsSseFallback(previousPlan);
    applyCesiumNativeKicking(previousPlan);
    evictUnusedSurfaceMeshes();
}

bool BasemapLayer::isCurrentDesiredTile(const TileKey& key) const {
    return std::find(layerPlan_.desiredTiles.begin(),
                     layerPlan_.desiredTiles.end(),
                     key) != layerPlan_.desiredTiles.end();
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

BasemapLayer::SurfaceGpuMesh*
BasemapLayer::getOrCreateSurfaceGpuMesh(const TileKey& key,
                                        const Rectangle& bounds,
                                        const TerrainLayer* terrainLayer) {
    if (!renderDevice_) return nullptr;

    const int gridSize = surfaceGridSizeForZoom(key.z);
    const TerrainTile* terrainTile = terrainLayer
        ? terrainLayer->findBestTileForBounds(bounds)
        : nullptr;
    const bool useTerrain = terrainTile && terrainTile->valid();
#ifdef __ANDROID__
    static int dbgCount = 0;
    if (terrainLayer && ++dbgCount <= 3) {
        __android_log_print(ANDROID_LOG_INFO, "BasemapLayer",
            "getOrCreate: z=%d useTerr=%d cacheTerr=%d",
            key.z, useTerrain, terrainLayer->cachedTileCount());
    }
#endif
    const std::string ck = tileCacheKey(key) + "/surface/" +
        (useTerrain
            ? "terrain/" + tileCacheKey(terrainTile->key())
            : "ellipsoid") +
        "/" + std::to_string(gridSize);

    auto found = surfaceMeshCache_.find(ck);
    if (found != surfaceMeshCache_.end()) {
        return &found->second;
    }

    constexpr double kTerrainSkirtHeightMeters = -100.0;

    // OpenGlobus equalizeVertices: get parent tile for edge averaging
    const TerrainTile* parentTile = nullptr;
    if (useTerrain && terrainTile) {
        TileKey parentKey = TilePlanBuilder::parentKey(terrainTile->key());
        parentTile = terrainLayer->findBestTileForKey(parentKey);
    }

    SurfaceTileMesh mesh = useTerrain
        ? TileSurface::buildTerrainMesh(
              bounds, terrainTile, gridSize, kTerrainSkirtHeightMeters, parentTile)
        : TileSurface::buildEllipsoidMesh(bounds, gridSize);
    if (mesh.vertices.empty() || mesh.indices.empty()) return nullptr;

    const Vec3 localOrigin = meshCentroid(mesh);
    std::vector<SurfaceGpuVertex> vertices =
        makeSurfaceGpuVertices(mesh, localOrigin);
    if (vertices.empty()) return nullptr;

    SurfaceGpuMesh gpuMesh;
    gpuMesh.localOriginEcef = localOrigin;
    gpuMesh.usesTerrain = useTerrain;
    gpuMesh.usesParentTerrain = useTerrain && terrainTile->key() != key;
    gpuMesh.terrainReady = useTerrain && terrainTile->key() == key;
    gpuMesh.terrainTransition = useTerrain && terrainTile->key() != key;

    BufferDesc vbDesc;
    vbDesc.size = vertices.size() * sizeof(SurfaceGpuVertex);
    vbDesc.data = vertices.data();
    vbDesc.usage = BufferDesc::Usage::Static;
    vbDesc.type = BufferDesc::Type::Vertex;
    gpuMesh.vertexBuffer = renderDevice_->createBuffer(vbDesc);
    if (!gpuMesh.vertexBuffer) return nullptr;

    BufferDesc ibDesc;
    ibDesc.size = mesh.indices.size() * sizeof(uint32_t);
    ibDesc.data = mesh.indices.data();
    ibDesc.usage = BufferDesc::Usage::Static;
    ibDesc.type = BufferDesc::Type::Index;
    gpuMesh.indexBuffer = renderDevice_->createBuffer(ibDesc);
    if (!gpuMesh.indexBuffer) return nullptr;

    // cesium-native alignment: per-vertex normals are used directly in the
    // shader (glTF NORMAL attribute style); no separate normal map texture
    // upload is required, eliminating per-tile RGBA8 texture allocation.

    // Water mask texture (QuantizedMesh extension ID=2)
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

    gpuMesh.indexCount = static_cast<int>(mesh.indices.size());
    auto [it, inserted] = surfaceMeshCache_.emplace(ck, std::move(gpuMesh));
    (void)inserted;
    return &it->second;
}

void BasemapLayer::evictUnusedSurfaceMeshes() {
    std::unordered_set<std::string> keep;
    keep.reserve(layerPlan_.renderTiles.size());
    for (const auto& renderTile : layerPlan_.renderTiles) {
        const std::string prefix = tileCacheKey(renderTile.targetKey) + "/surface/";
        for (const auto& entry : surfaceMeshCache_) {
            if (entry.first.rfind(prefix, 0) == 0) {
                keep.insert(entry.first);
            }
        }
    }

    for (auto it = surfaceMeshCache_.begin(); it != surfaceMeshCache_.end(); ) {
        if (keep.find(it->first) == keep.end()) {
            it = surfaceMeshCache_.erase(it);
        } else {
            ++it;
        }
    }
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

void BasemapLayer::buildRenderCommands(Renderer& renderer,
                                        const TerrainLayer* terrainLayer,
                                        RenderCommandList& commands) {
    if (!visible_) return;

    for (const auto& renderTile : layerPlan_.renderTiles) {
        const TileKey& key = renderTile.targetKey;
        const TileKey& textureKey = renderTile.textureKey;
        Texture* tex = textureCache_.get(textureKey);

        if (!tex) continue;

        Rectangle bounds = tileScheme_->tileToRectangle(key);
        Rectangle textureBounds = tileScheme_->tileToRectangle(textureKey);
        TileTextureWindow uv = TileSurface::textureWindow(bounds, textureBounds);
        if (tex->width() > 0 && tex->height() > 0) {
            const float insetU = 0.5f / static_cast<float>(tex->width());
            const float insetV = 0.5f / static_cast<float>(tex->height());
            uv.offsetU += insetU;
            uv.offsetV += insetV;
            uv.scaleU = std::max(0.0f, uv.scaleU - insetU * 2.0f);
            uv.scaleV = std::max(0.0f, uv.scaleV - insetV * 2.0f);
        }

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

        SurfaceGpuMesh* gpuMesh = getOrCreateSurfaceGpuMesh(key, bounds, terrainLayer);
        if (!gpuMesh) continue;

        auto cmd = renderer.makeSurfaceTileCommand(
            attachment.texture,
            gpuMesh->waterMaskTexture.get(),
            gpuMesh->vertexBuffer.get(),
            gpuMesh->indexBuffer.get(),
            gpuMesh->indexCount,
            attachment.uvOffsetU,
            attachment.uvOffsetV,
            attachment.uvScaleU,
            attachment.uvScaleV);
        cmd.uniforms["u_tileOpacity"] = {attachment.opacity};
        cmd.uniforms["u_transitionOpacity"] = {renderTile.transitionOpacity};
        const float effectiveOpacity =
            attachment.opacity * std::clamp(renderTile.transitionOpacity, 0.0f, 1.0f);
        cmd.blend = effectiveOpacity < 0.999f;
        cmd.uniforms["u_surfaceGeneration"] = {
            static_cast<float>(generation_)
        };
        const Vec3 cameraRelativeToTileOrigin =
            lastCameraPosition_ - gpuMesh->localOriginEcef;
        cmd.uniforms["u_cameraRelativeOrigin"] = {
            static_cast<float>(cameraRelativeToTileOrigin.x()),
            static_cast<float>(cameraRelativeToTileOrigin.y()),
            static_cast<float>(cameraRelativeToTileOrigin.z())
        };
        // Pass tile centroid for GPU geodetic normal computation
        cmd.uniforms["u_tileOrigin"] = {
            static_cast<float>(gpuMesh->localOriginEcef.x()),
            static_cast<float>(gpuMesh->localOriginEcef.y()),
            static_cast<float>(gpuMesh->localOriginEcef.z())
        };
        cmd.frameId = layerPlan_.frameId;
        cmd.generation = generation_;

        commands.push_back(std::move(cmd));
    }
}

} // namespace earth_engine
