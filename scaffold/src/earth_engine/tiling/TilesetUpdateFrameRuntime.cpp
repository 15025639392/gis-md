#include "TilesetUpdateFrameRuntime.h"

#include "TileContentAccess.h"
#include "TileContentLifecycleManager.h"
#include "Tileset.h"
#include "TilesetProviderDiagnosticsCollector.h"
#include "TilesetSelectionFrameFacade.h"

namespace earth_engine {

namespace {

constexpr double kPostInteractionResourceSmoothingSeconds = 1.25;

} // namespace

TilesetUpdateFrameRuntimeResult TilesetUpdateFrameRuntime::run(
    Tileset& tileset,
    const FrameState& frameState) {
    // cesium-native: increment generation each frame so that
    // RenderCommand validator (non-zero check) accepts SurfaceTile commands.
    ++tileset.generation_;

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
            TilesetProviderDiagnosticsCollector::collect(
                tileset.terrainProvider_.get(),
                tileset.contentProvider_.get(),
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
        [&tileset](bool uploadInteractionActive,
                   bool uploadResourceSmoothingActive,
                   FrameResourceBudget* budget) {
            return tileset.processPendingContentUploads(
                uploadInteractionActive,
                uploadResourceSmoothingActive,
                budget);
        },
        [&tileset]() {
            tileset.markContentResourcesDirty();
        },
        [&tileset]() {
            return tileset.contentLifecycle_.hasPendingWork();
        },
        [&tileset]() {
            TilesetSelectionFrameFacade::refreshTilePlanRenderEntries(tileset);
        },
        [&tileset](const FrameState& selectionFrameState) {
            TilesetSelectionFrameFacade::selectTiles(
                tileset,
                selectionFrameState);
        },
        [&tileset](const TileKey& key) {
            return tileset.contentAccess_.ensureTile(key);
        },
        [&tileset](const std::vector<TileLoadRequest>& requests,
                   FrameResourceBudget* budget) {
            return tileset.requestMissingContent(
                requests,
                budget);
        });
    const TileUpdateUploadRunResult& uploadWork = frameWork.uploadWork;
    const TileUpdateSelectionWorkResult& selectionWork =
        frameWork.selectionWork;
    return TilesetUpdateFrameRuntimeResult{
        frameWork,
        TileUpdateDebugLogInput{
            tileset.tilePlan_.visibleTiles.size(),
            tileset.loadQueue_.size(),
            selectionWork.computeMs,
            selectionWork.prefetchMs,
            selectionWork.requestMs,
            uploadWork.terrainUploadMs,
            uploadWork.rasterUploadMs,
            tileset.contentLifecycle_.terrainCache().size(),
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
