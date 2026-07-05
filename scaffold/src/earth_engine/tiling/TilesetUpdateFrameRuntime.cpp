#include "TilesetUpdateFrameRuntime.h"

#include "../scene/Camera.h"
#include "TileContentAccess.h"
#include "TileContentLifecycleManager.h"
#include "TileFrameWorkCoordinator.h"
#include "TileRenderPlanFrameRefresher.h"
#include "Tileset.h"
#include "TilesetProviderDiagnosticsCollector.h"
#include "TilesetSelectionFrameFacade.h"
#include "../debug/PlatformLog.h"

namespace earth_engine {

namespace {

constexpr double kPostInteractionResourceSmoothingSeconds = 1.25;

} // namespace

TilesetUpdateFrameRuntimeResult TilesetUpdateFrameRuntime::run(
    Tileset& tileset,
    const FrameState& frameState,
    IPrepareRendererResources* pPrepRenderer) {
    // cesium-native: increment generation each frame so that
    // RenderCommand validator (non-zero check) accepts SurfaceTile commands.
    ++tileset.generation_;

    // Land any finished async-worker selection BEFORE the reuse decision. The
    // reuse gate doesn't know the worker has a pending result, so leaving this
    // to the reuse-gated selectTiles path deadlocks a static-camera bootstrap
    // (empty plan → no loads → revision static →永远 reuse). No-op unless the
    // true-async worker path is active with a ready result.
    TilesetSelectionFrameFacade::consumeAsyncSelectionResult(tileset);

    TileFrameWorkResult frameWork = TileFrameWorkCoordinator::run(
        TileFrameWorkInput{
            tileset.tilePlan_,
            tileset.loadQueue_,
            tileset.selectionCounters_,
            tileset.selectionReuseState_,
            tileset.rasterOverlays_,
            tileset.frameResourceBudget_,
            tileset.device_,
            frameState,
            tileset.resourceRevision_,
            tileset.options_.maximumSimultaneousTileLoads,
            TilesetProviderDiagnosticsCollector::collectContentAndRaster(
                tileset.terrainProviders_.contentProvider(),
                tileset.rasterOverlays_)
                .maximumTransportActiveRequests(
                    TileFrameResourceBudgetPlanInput::
                        kDefaultMaximumTransportActiveRequests),
            tileset.options_.mainThreadLoadingTimeLimit,
            kPostInteractionResourceSmoothingSeconds,
            tileset.options_.maximumScreenSpaceError},
        TileFrameWorkState{
            tileset.cameraMoving_,
            tileset.interactionActiveForFrame_,
            tileset.resourceSmoothingActiveForFrame_,
            tileset.lastInteractionActiveTimeSeconds_,
            tileset.lastCameraPosition_,
            tileset.lastCameraDirection_},
        [&tileset, pPrepRenderer](bool uploadInteractionActive,
                                  bool uploadResourceSmoothingActive,
                                  FrameResourceBudget* budget) {
            return tileset.processPendingLoads(
                uploadInteractionActive,
                uploadResourceSmoothingActive,
                pPrepRenderer,
                budget);
        },
        [&tileset]() {
            tileset.markContentResourcesDirty();
        },
        [&tileset]() {
            return tileset.contentLifecycle_.hasPendingWork();
        },
        [&tileset]() {
            TileRenderPlanFrameRefresher::refresh(
                tileset.tilePlan_,
                tileset.contentAccess_,
                tileset.rasterOverlays_,
                TileRenderPlanFrameRefreshOptions{
                    tileset.options_.enableLodTransitionPeriod,
                    tileset.interactionActiveForFrame_,
                    tileset.resourceSmoothingActiveForFrame_});
        },
        [&tileset](const FrameState& selectionFrameState) {
            TilesetSelectionFrameFacade::selectTiles(
                tileset,
                selectionFrameState);
        },
        [&tileset](const TileKey& key) {
            return tileset.contentAccess_.ensureTile(key);
        },
        [&tileset, pPrepRenderer](TilesetTile& tile) {
            tileset.cacheOwnership_.unloadTileContent(tile, pPrepRenderer);
        },
        [&tileset, pPrepRenderer](const std::vector<TileLoadRequest>& requests,
                                  FrameResourceBudget* budget) {
            return tileset.requestMissingContent(
                requests,
                budget,
                pPrepRenderer);
        });
    // Drain the async GPU upload queue.  Terrain CPU work dispatched to
    // worker threads by processPendingLoads lands here for GPU upload.
    {   const double t_drain = perf::nowMs();
        tileset.drainGpuUploadQueue(pPrepRenderer);
        const double drainMs = perf::nowMs() - t_drain;
        if (drainMs > 1.0) {
            platformLog(LogLevel::Info, "EarthPerf",
                "drainGpuUpload: %.2f ms", drainMs);
        }
    }

    const TileUpdateUploadRunResult& uploadWork = frameWork.uploadWork;
    const TileUpdateSelectionWorkResult& selectionWork =
        frameWork.selectionWork;
    return TilesetUpdateFrameRuntimeResult{
        TileUpdateDebugLogInput{
            tileset.tilePlan_.visibleTiles.size(),
            tileset.loadQueue_.size(),
            selectionWork.computeMs,
            selectionWork.prefetchMs,
            selectionWork.requestMs,
            uploadWork.terrainUploadMs,
            uploadWork.rasterUploadMs,
            static_cast<size_t>(
                tileset.cachedHeightmapTerrainTilesForLegacySurfacePath()),
            tileset.contentLifecycle_.loadLifecycle()
                .requestState()
                .totalRequestCount(),
            tileset.selectionCounters_,
            selectionWork.reuseMode,
            selectionWork.reuseRejectReason,
            selectionWork.reusedSelection,
            uploadWork.rasterUploadsProcessed,
            frameWork.interactionActive,
            frameWork.resourceSmoothingActive}};
}

} // namespace earth_engine
