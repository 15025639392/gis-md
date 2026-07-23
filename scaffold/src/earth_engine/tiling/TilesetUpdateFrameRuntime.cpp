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
    const bool acceptedAsyncSelection =
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
            tileset.options_.maximumScreenSpaceError,
            tileset.options_.cullRequestsWhileMoving,
            tileset.options_.cullRequestsWhileMovingMultiplier,
            tileset.options_.enableTerrainFillProxy,
            tileset.options_.terrainFillProxyGridSize,
            tileset.hasTerrainQuadtree(),
            pPrepRenderer,
            // P5b:hold 期间禁 reuse——见 TileSelectionReuseInput::presentationHeld。
            tileset.shouldHoldPresentationFrame()},
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
                    tileset.resourceSmoothingActiveForFrame_,
                    tileset.options_.maximumScreenSpaceError});
        },
        [&tileset, pPrepRenderer](
            const FrameState& selectionFrameState,
            TileSelectionPerformanceTimings& selectionTimings) {
            TilesetSelectionFrameFacade::selectTiles(
                tileset,
                selectionFrameState,
                &selectionTimings,
                pPrepRenderer);
        },
        [&tileset](const TileKey& key) {
            return tileset.tileRegistry_.findTile(key);
        },
        [&tileset](const TileKey& key) {
            return tileset.contentAccess_.ensureTile(key);
        },
        [&tileset, pPrepRenderer](TilesetTile& tile) {
            tileset.cacheOwnership_.unloadTileContent(tile, pPrepRenderer);
        },
        [&tileset, pPrepRenderer](TilesetTile& tile) {
            tileset.rasterUpsampledChildren_
                .createRasterOverlayUpsampledChildren(
                    tile,
                    pPrepRenderer,
                    tileset.options_.decoupleImageryFromGeometry);
        },
        [&tileset, pPrepRenderer](TileLoadQueue& requests,
                                  FrameResourceBudget* budget) {
            return tileset.requestMissingContent(
                requests,
                budget,
                pPrepRenderer);
        },
        [&tileset](TilesetTile& tile) {
            tileset.resourceInvalidator_.reconcileTileResources(tile);
        });
    if (frameWork.selectionWork.reusedSelection &&
        !acceptedAsyncSelection) {
        tileset.retirePreviousSelectionReferencesForReuse();
    }
    // Drain the async GPU upload queue.  Terrain CPU work dispatched to
    // worker threads by processPendingLoads lands here for GPU upload.
    double gpuUploadDrainMs = 0.0;
    {
        const double t_drain = perf::nowMs();
        tileset.drainGpuUploadQueue(pPrepRenderer);
        gpuUploadDrainMs = perf::nowMs() - t_drain;
        if (gpuUploadDrainMs > 1.0) {
            platformLog(LogLevel::Info, "EarthPerf",
                "drainGpuUpload: %.2f ms", gpuUploadDrainMs);
        }
    }
    if ((frameState.frameId % 120u) == 0u) {
        platformLog(
            LogLevel::Info,
            "EarthPerf",
            "frame=%llu scope=DeferredCpuRelease pendingBytes=%lld "
            "pendingTasks=%u limitBytes=%lld",
            static_cast<unsigned long long>(frameState.frameId),
            static_cast<long long>(
                GltfRenderResourcePreparer::
                    deferredCpuReleasePendingBytes()),
            GltfRenderResourcePreparer::
                deferredCpuReleasePendingTasks(),
            static_cast<long long>(
                GltfRenderResourcePreparer::
                    deferredCpuReleaseLimitBytes()));
    }

    const TileUpdateUploadRunResult& uploadWork = frameWork.uploadWork;
    const TileUpdateSelectionWorkResult& selectionWork =
        frameWork.selectionWork;
    const TilesetProviderDiagnosticsSnapshot rasterDiagnostics =
        TilesetProviderDiagnosticsCollector::collectContentAndRaster(
            tileset.terrainProviders_.contentProvider(),
            tileset.rasterOverlays_);
    return TilesetUpdateFrameRuntimeResult{
        TileUpdateDebugLogInput{
            tileset.tilePlan_.visibleTiles.size(),
            tileset.loadQueue_.size(),
            selectionWork.computeMs,
            selectionWork.selectorTraversalMs,
            selectionWork.selectorRefineMs,
            selectionWork.selectorRenderPlanMs,
            selectionWork.selectorRequestPlanningMs,
            selectionWork.prefetchMs,
            selectionWork.prefetchBaseMs,
            selectionWork.prefetchFillMs,
            selectionWork.prefetchVisibleMs,
            selectionWork.prefetchLoadQueueMs,
            selectionWork.prefetchAdvanceMs,
            selectionWork.prefetchMapMs,
            selectionWork.prefetchEarlyMapMs,
            selectionWork.requestMs,
            uploadWork.terrainUploadMs,
            uploadWork.rasterUploadMs,
            uploadWork.rasterSelectTaskMs,
            uploadWork.rasterUploadTextureMs,
            uploadWork.rasterTileFinalizeMs,
            uploadWork.rasterBookkeepingMs,
            static_cast<size_t>(
                tileset.cachedHeightmapTerrainTilesForLegacySurfacePath()),
            tileset.contentLifecycle_.loadLifecycle()
                .requestState()
                .totalRequestCount(),
            rasterDiagnostics.rasterSourceRequestsInFlight,
            rasterDiagnostics.rasterActiveMappedSourceSets,
            rasterDiagnostics.rasterPendingSourceFallbacks,
            rasterDiagnostics.rasterInFlightSourceTiles,
            rasterDiagnostics.rasterInFlightSourceWaiters,
            rasterDiagnostics.rasterPendingUploads,
            tileset.selectionCounters_,
            selectionWork.reuseMode,
            selectionWork.reuseRejectReason,
            selectionWork.reusedSelection,
            selectionWork.prefetchVisibleTiles,
            selectionWork.prefetchLoadQueueTiles,
            selectionWork.prefetchAdvanceCount,
            selectionWork.prefetchMapCount,
            selectionWork.prefetchEarlyMapCount,
            selectionWork.prefetchVisibleEarlyMapCount,
            selectionWork.prefetchLoadQueueEarlyMapCount,
            selectionWork.prefetchEarlyMapBudgetExhausted,
            uploadWork.rasterUploadsProcessed,
            uploadWork.rasterMappedUploadsProcessed,
            uploadWork.rasterUploadMaxMs,
            uploadWork.rasterUploadMaxWidth,
            uploadWork.rasterUploadMaxHeight,
            frameWork.interactionActive,
            frameWork.resourceSmoothingActive,
            selectionWork.selectorRefineOverlayMs,
            selectionWork.selectorRefineDecisionMs,
            selectionWork.selectorRefineMaterializeMs,
            selectionWork.selectorRefineCommitMs,
            selectionWork.selectorDetailedTimings,
            uploadWork.rasterSourceFallbackMs,
            uploadWork.rasterSourceSnapshotMs,
            uploadWork.rasterSourceIssueMs,
            uploadWork.rasterUploadQueueSelectMs,
            selectionWork.selectorRefineMaterializeCalls,
            selectionWork.selectorRefineMaterializeChanged,
            selectionWork.selectorRefineMaterializeRetry,
            selectionWork.selectorRefineMaterializeFastPath,
            selectionWork.selectorVisitVisibilityMs,
            selectionWork.selectorVisitInputMetricsMs,
            selectionWork.selectorVisitPolicyMs,
            selectionWork.prefetchRenderPlanMs,
            selectionWork.prefetchRenderPlanUpdateMs,
            selectionWork.prefetchRenderPlanActionMs,
            selectionWork.prefetchRenderPlanTiles,
            selectionWork.prefetchRenderPlanAuthoritativeUpdates,
            selectionWork.prefetchRenderPlanStableReuses,
            gpuUploadDrainMs,
            selectionWork.requestOutcome.classifiedContent,
            selectionWork.requestOutcome
                .classifiedTerrainAvailabilityUpsample,
            selectionWork.requestOutcome.classifiedRasterDetailUpsample,
            selectionWork.requestOutcome.issuedContent,
            selectionWork.requestOutcome
                .issuedTerrainAvailabilityUpsample,
            selectionWork.requestOutcome.issuedRasterDetailUpsample,
            selectionWork.requestOutcome
                .skippedUpsampleWorkerCapacity,
            selectionWork.requestOutcome.skippedMotionCull,
            tileset.totalBytesUsed(),
            tileset.contentBytesUsed(),
            tileset.imageryTextureBytesUsed()}};
}

} // namespace earth_engine
