#include "Tileset.h"
#include "../scene/FrameState.h"
#include "../scene/Camera.h"
#include "../renderer/Renderer.h"
#include "../renderer/RenderDevice.h"
#include "../tiling/LoadedTerrainHeightSampler.h"
#include "../tiling/TileCacheKey.h"
#include "../tiling/TileFrameResourceBudgetPlanner.h"
#include "../tiling/TileOcclusionResolver.h"
#include "../tiling/TileRenderFrameContext.h"
#include "../tiling/TileRenderReferenceReleaser.h"
#include "../tiling/TileRasterOverlayReadinessPolicy.h"
#include "../tiling/TileSelectionStateResetter.h"
#include "../tiling/TileSelectionWorker.h"
#include "../tiling/TileSoftwareOcclusionPolicy.h"
#include "../tiling/TilesetProviderDiagnosticsCollector.h"
#include "../tiling/TilesetRenderFrameExecutor.h"
#include "../tiling/TilesetUpdateFrameFacade.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "../debug/PlatformLog.h"

#include <cassert>
#include <memory>
#include <optional>
#include <utility>

namespace earth_engine {

namespace {

constexpr int kSmoothedMainThreadUploadLimit = 4;

FrameResourceBudgetConfig makeFrameResourceBudgetConfig(
    const TilesetOptions& options,
    bool interactionActive,
    bool resourceSmoothingActive) {
    return TileFrameResourceBudgetPlanner::plan(
        TileFrameResourceBudgetPlanInput::withDefaultTransport(
            options.maximumSimultaneousTileLoads,
            options.mainThreadLoadingTimeLimit,
            interactionActive,
            resourceSmoothingActive));
}

} // namespace

Tileset::Tileset(std::unique_ptr<TileScheme> tileScheme,
                 std::vector<ActivatedRasterOverlay*> rasterOverlays,
                 RenderDevice* device,
                 TilesetOptions options)
    : Tileset(
          TilesetTerrainProviders(nullptr),
          std::move(tileScheme),
          std::move(rasterOverlays),
          device,
          std::move(options)) {}

Tileset::Tileset(TilesetTerrainProviders terrainProviders,
                 std::unique_ptr<TileScheme> tileScheme,
                 std::vector<ActivatedRasterOverlay*> rasterOverlays,
                 RenderDevice* device,
                 TilesetOptions options)
    : terrainProviders_(std::move(terrainProviders)),
      tileScheme_(std::move(tileScheme)),
      rasterOverlays_(std::move(rasterOverlays)),
      device_(device),
      options_(std::move(options)),
      resourceInvalidator_(
          resourceRevision_,
          contentCache_),
      contentAccess_(
          terrainProviders_.contentProviderOwnsTerrainQuadtree()
              ? TileContentAccess::forContentTerrain(
                    tileRegistry_,
                    *tileScheme_,
                    *terrainProviders_.contentProvider(),
                    &resourceInvalidator_)
              : TileContentAccess::forNoTerrain(
                    tileRegistry_,
                    *tileScheme_,
                    terrainProviders_.contentProvider(),
                    &resourceInvalidator_)),
      cacheOwnership_(
          contentCache_,
          contentLifecycle_,
          loadQueue_,
          tileRegistry_.tiles(),
          rasterOverlays_,
          resourceSmoothingActiveForFrame_,
          options_.maximumCachedBytes,
          options_.tileCacheUnloadTimeLimit,
          &gpuUploadQueue_),
      rasterUpsampledChildren_(
          contentAccess_,
          resourceInvalidator_),
      meshPreparation_(
          contentLifecycle_,
          resourceInvalidator_,
          loadQueue_,
          terrainProviders_.hasTerrainQuadtree(),
          device_,
          rasterOverlays_),
      contentRuntime_(
          contentLifecycle_,
          contentAccess_,
          meshPreparation_,
          resourceInvalidator_),
      renderCommands_(
          meshPreparation_,
          resourceInvalidator_,
          rasterOverlays_,
          device_) {
    frameResourceBudget_.beginFrame(
        0,
        makeFrameResourceBudgetConfig(options_, false, false));
    for (ActivatedRasterOverlay* overlay : rasterOverlays_) {
        if (overlay) {
            overlay->ensureTileProvider(device_);
        }
    }
}

Tileset::Tileset(
    std::unique_ptr<TileScheme> tileScheme,
    std::vector<ActivatedRasterOverlay*> rasterOverlays,
    RenderDevice* device,
    TilesetOptions options,
    std::unique_ptr<TilesetContentProvider> contentProvider)
    : Tileset(
          TilesetTerrainProviders(std::move(contentProvider)),
          std::move(tileScheme),
          std::move(rasterOverlays),
          device,
          std::move(options)) {}

bool Tileset::hasTerrainQuadtree() const {
    return terrainProviders_.hasTerrainQuadtree();
}

Tileset::~Tileset() {
    discardPendingRenderReferences();
    releaseSelectionReferences(selectionActiveTiles_);
    releaseSelectionReferences(selectionActiveTilesPrev_);
    // cesium-native keeps TilesetContentManager alive while worker callbacks
    // complete. This local engine has synchronous destruction, so wait until
    // every callback has observed the destroyed state and left the callback.
    contentLifecycle_.shutdown();
}

int Tileset::pendingRequests() const {
    return contentLifecycle_.pendingRequests();
}

int Tileset::cachedHeightmapTerrainTilesForLegacySurfacePath() const {
    return 0;
}

int64_t Tileset::totalBytesUsed() const {
    return cacheOwnership_.totalBytesUsed();
}

int64_t Tileset::contentBytesUsed() const {
    return cacheOwnership_.contentBytesUsed();
}

int64_t Tileset::imageryTextureBytesUsed() const {
    return cacheOwnership_.imageryTextureBytesUsed();
}

void Tileset::setOcclusionCallback(OcclusionCallback callback) {
    occlusionCallback_ = std::move(callback);
}

void Tileset::clearOcclusionCallback() {
    occlusionCallback_ = nullptr;
}

TilesetLoadDiagnostics Tileset::loadDiagnostics() const {
    TilesetLoadDiagnostics diagnostics = TileLoadDiagnosticsCollector::collect(
        loadQueue_,
        contentLifecycle_.loadLifecycle(),
        frameResourceBudget_,
        contentCache_.unloadQueue(),
        tileRegistry_.tiles());
    TilesetProviderDiagnosticsCollector::collectContentAndRaster(
        terrainProviders_.contentProvider(),
        rasterOverlays_)
        .applyTo(diagnostics);
    diagnostics.lastRequestOutcome = lastRequestOutcome_;
    return diagnostics;
}

TileContentRuntimeRequestFrame
Tileset::makeContentRuntimeRequestFrame(
    IPrepareRendererResources* pPrepRenderer) {
    TileContentRuntimeRequestFrame frame{
        rasterOverlays_,
        tileRegistry_.tiles()};
    frame.contentProvider = terrainProviders_.contentProvider();
    frame.device = device_;
    frame.pPrepRenderer = pPrepRenderer;
    frame.frameNumber = frameNumber_;
    frame.maximumSimultaneousTileLoads =
        options_.maximumSimultaneousTileLoads;
    frame.mainThreadLoadingTimeLimit = options_.mainThreadLoadingTimeLimit;
    frame.currentFrameTimeSeconds = currentFrameTimeSeconds_;
    frame.smoothedMainThreadUploadLimit =
        static_cast<uint32_t>(kSmoothedMainThreadUploadLimit);
    return frame;
}

TileContentRuntimeUploadFrame Tileset::makeContentRuntimeUploadFrame(
    IPrepareRendererResources* pPrepRenderer) {
    TileContentRuntimeUploadFrame frame{rasterOverlays_};
    frame.contentProvider = terrainProviders_.contentProvider();
    frame.device = device_;
    frame.pPrepRenderer = pPrepRenderer;
    frame.gpuUploadQueue = &gpuUploadQueue_;
    frame.frameNumber = frameNumber_;
    frame.maximumSimultaneousTileLoads =
        options_.maximumSimultaneousTileLoads;
    frame.mainThreadLoadingTimeLimit = options_.mainThreadLoadingTimeLimit;
    frame.currentFrameTimeSeconds = currentFrameTimeSeconds_;
    frame.smoothedMainThreadUploadLimit =
        static_cast<uint32_t>(kSmoothedMainThreadUploadLimit);
    return frame;
}

TileLoadRequestOutcome Tileset::requestMissingContent(
    const std::vector<TileLoadRequest>& loadRequests,
    FrameResourceBudget* budget,
    IPrepareRendererResources* pPrepRenderer) {
    lastRequestOutcome_ = contentRuntime_.requestMissingTiles(
        loadRequests,
        makeContentRuntimeRequestFrame(pPrepRenderer),
        budget);
    return lastRequestOutcome_;
}

TileLoadRequestOutcome Tileset::requestMissingContent(
    TileLoadQueue& loadQueue,
    FrameResourceBudget* budget,
    IPrepareRendererResources* pPrepRenderer) {
    lastRequestOutcome_ = contentRuntime_.requestMissingTiles(
        loadQueue,
        makeContentRuntimeRequestFrame(pPrepRenderer),
        budget);
    return lastRequestOutcome_;
}

bool Tileset::processPendingLoads(
    bool interactionActive,
    bool resourceSmoothingActive,
    IPrepareRendererResources* pPrepRenderer,
    FrameResourceBudget* budget) {
    return contentRuntime_.processPendingUploads(
        makeContentRuntimeUploadFrame(pPrepRenderer),
        interactionActive,
        resourceSmoothingActive,
        budget);
}

bool Tileset::drainGpuUploadQueue(
    IPrepareRendererResources* pPrepRenderer,
    uint32_t maxUploadsPerFrame) {
    return contentRuntime_.drainGpuUploadQueue(
        makeContentRuntimeUploadFrame(pPrepRenderer),
        &frameResourceBudget_,
        maxUploadsPerFrame);
}

void Tileset::markContentResourcesDirty() {
    contentRuntime_.markResourcesDirty();
}

TileOcclusionState Tileset::checkSingleTileOcclusion(
    const TilesetTile& tile) const {
    if (occlusionCallback_) {
        return occlusionCallback_(tile);
    }
    // 相机派生量（迭代法测地转换等）按相机位置记忆化：一帧内逐瓦片
    // 调用共享同一份 CameraContext（P2-11）。
    const Vec3& cameraPosition = lastCameraPosition_;
    if (!occlusionCameraContextValid_ ||
        occlusionCameraContext_.cameraPosition.x() != cameraPosition.x() ||
        occlusionCameraContext_.cameraPosition.y() != cameraPosition.y() ||
        occlusionCameraContext_.cameraPosition.z() != cameraPosition.z()) {
        occlusionCameraContext_ =
            TileSoftwareOcclusionPolicy::CameraContext::fromCameraPosition(
                cameraPosition);
        occlusionCameraContextValid_ = true;
    }
    return TileSoftwareOcclusionPolicy::check(tile, occlusionCameraContext_);
}

TileOcclusionState Tileset::checkOcclusion(const TilesetTile& tile) const {
    return TileOcclusionResolver::check(
        tile,
        [this](const TilesetTile& occlusionTile) {
            return checkSingleTileOcclusion(occlusionTile);
        });
}

void Tileset::rotateSelectionActiveTiles(bool resetSelectionState) {
    ++selectionActiveFrameId_;
    std::swap(selectionActiveTiles_, selectionActiveTilesPrev_);

    if (resetSelectionState) {
        auto resetOnce = [this](TilesetTile* tile) {
            if (!tile ||
                tile->selectionActiveFrameId == selectionActiveFrameId_) {
                return;
            }
            tile->selectionActiveFrameId = selectionActiveFrameId_;
            TileSelectionStateResetter::resetOne(*tile, rasterOverlays_);
        };

        // The vector that became current is two traversals old. Reset it once
        // more to decay stale previousSelectionState before releasing it. A
        // stable tile is also in the previous vector, and the frame stamp keeps
        // that overlap from shifting its selection history twice.
        for (TilesetTile* tile : selectionActiveTiles_) {
            resetOnce(tile);
        }
        for (TilesetTile* tile : selectionActiveTilesPrev_) {
            resetOnce(tile);
        }
    }

    // beginTraversal(): previous.swap(current), current.clear(). Clearing the
    // intrusive-pointer vector releases only the two-traversals-old ownership;
    // previous traversal references remain live while this frame is selected.
    releaseSelectionReferences(selectionActiveTiles_);
}

void Tileset::retirePreviousSelectionReferencesForReuse() {
    releaseSelectionReferences(selectionActiveTilesPrev_);
}

void Tileset::trackSelectionActiveTile(
    TilesetTile& tile,
    bool resetSelectionState) {
    if (resetSelectionState &&
        tile.selectionActiveFrameId != selectionActiveFrameId_) {
        tile.selectionActiveFrameId = selectionActiveFrameId_;
        TileSelectionStateResetter::resetOne(tile, rasterOverlays_);
    }
    if (tile.selectionTraversalFrameId == selectionActiveFrameId_) {
        return;
    }

    tile.selectionTraversalFrameId = selectionActiveFrameId_;
    selectionActiveTiles_.push_back(&tile);
    tile.addReference();
    cacheOwnership_.markIneligibleForUnloading(
        TileCacheKey::forTile(tile.key));
}

void Tileset::releaseSelectionReferences(
    std::vector<TilesetTile*>& tiles) {
    for (TilesetTile* tile : tiles) {
        if (!tile) {
            continue;
        }
        assert(tile->referenceCount() > 0);
        tile->removeReference();
        cacheOwnership_.markEligibleForUnloading(
            tile,
            TileCacheKey::forTile(tile->key));
    }
    tiles.clear();
}

void Tileset::resetActiveSelectionState() {
    rotateSelectionActiveTiles(true);
}

void Tileset::onSelectionVisitTile(TilesetTile& tile) {
    trackSelectionActiveTile(tile, true);
}

std::optional<float> Tileset::sampleHeightOptional(
    double lngRad, double latRad) const {
    return LoadedTerrainHeightSampler::sampleHeightOptional(
        tileRegistry_.tiles(),
        lngRad,
        latRad);
}

void Tileset::update(
    const FrameState& frameState,
    IPrepareRendererResources* pPrepRenderer) {
    const double t0 = perf::nowMs();
    TilesetUpdateFrameFacade::update(*this, frameState, pPrepRenderer);
    const double t_tot = perf::nowMs() - t0;
    if (t_tot > 5.0) {
        platformLog(LogLevel::Info, "EarthPerf",
            "Tileset.update_real: %.2f ms", t_tot);
    }
}

void Tileset::buildRenderCommands(Renderer& renderer,
                                  RenderCommandList& commands,
                                  uint64_t renderFrameId,
                                  const std::vector<TileRenderEntry>*
                                      renderEntriesOverride) {
    if (!pendingRenderReferences_.empty()) {
        platformLog(
            LogLevel::Error,
            "Tileset",
            "buildRenderCommands rejected: the previous command batch still "
            "owns render references; submit it or explicitly discard it");
        return;
    }
    ++frameNumber_;
    const uint64_t commandFrameNumber =
        renderFrameId != 0 ? renderFrameId : frameNumber_;
    renderCommands_.beginFrame(
        commandFrameNumber,
        generation_,
        currentFrameTimeSeconds_);
    TilesetRenderFrameExecutor::buildRenderCommands(
        TileRenderFrameContext{
            TileRenderFrameCoordinatorInput{
                tilePlan_,
                rasterOverlays_,
                commandFrameNumber,
                lastCameraPosition_,
                options_.fogDensityTable,
                selectionCounters_.fogCulled,
                resourceSmoothingActiveForFrame_,
                interactionActiveForFrame_,
                renderEntriesOverride},
            renderCommands_,
            cacheOwnership_,
            pendingRenderReferences_},
        renderer,
        commands);
}

bool Tileset::shouldHoldPresentationFrame() const {
    const bool hasSelectedSurfaceWork =
        !tilePlan_.visibleTiles.empty() || !tilePlan_.tilesFadingOut.empty();
    if (!hasSelectedSurfaceWork ||
        !requiresBaseImageryPresentationSurface()) {
        return false;
    }

    if (tilePlan_.renderEntries.empty()) {
        return true;
    }

    return !plannedRenderEntriesHaveRequiredBaseImagery();
}

bool Tileset::requiresBaseImageryPresentationSurface() const {
    const bool hasSelectedSurfaceWork =
        !tilePlan_.visibleTiles.empty() || !tilePlan_.tilesFadingOut.empty();
    if (!hasSelectedSurfaceWork) {
        return false;
    }

    for (const ActivatedRasterOverlay* activeOverlay : rasterOverlays_) {
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        const RasterOverlay& overlay = activeOverlay->getOverlay();
        if (overlay.role() == RasterOverlayRole::BaseImagery &&
            overlay.blocksCompleteRenderable()) {
            return true;
        }
    }

    return false;
}

bool Tileset::plannedRenderEntriesHaveRequiredBaseImagery() const {
    for (const TileRenderEntry& entry : tilePlan_.renderEntries) {
        const TilesetTile* renderTile =
            tileRegistry_.findTile(entry.renderKey);
        if (!renderTile) {
            return false;
        }
        if (!TileRasterOverlayReadinessPolicy::
                terrainSurfaceImageryDrawableReady(
                    *renderTile,
                    rasterOverlays_)) {
            return false;
        }
    }
    return true;
}

void Tileset::releaseRenderReferences() {
    // cesium-native: called from Scene after renderer_->submit().
    // Drops the reference that was added in buildRenderCommands for
    // tiles in the current render list, making them eligible for
    // unloading in the next frame.
    discardPendingRenderReferences();
}

void Tileset::discardPendingRenderReferences() {
    TileRenderReferenceReleaser::release(
        pendingRenderReferences_,
        [this](const TilesetTile* tile, const std::string& cacheKey) {
            cacheOwnership_.markEligibleForUnloading(tile, cacheKey);
        });
}

} // namespace earth_engine
