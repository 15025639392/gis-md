#include "TilesetUpdateFrameFacade.h"

#include "TileContentAccess.h"
#include "TileContentLifecycleManager.h"
#include "TileContentCacheManager.h"
#include "TileFrameDebugLogFormatter.h"
#include "../scene/Camera.h"
#include "TileFrameWorkCoordinator.h"
#include "Tileset.h"
#include "TilesetQueryFacade.h"
#include "TilesetSelectionFrameFacade.h"

#include "../debug/PerfTimer.h"
#include "../scene/FrameState.h"

#include <array>
#include <vector>

namespace earth_engine {

namespace {

constexpr double kPostInteractionResourceSmoothingSeconds = 1.25;

} // namespace

void TilesetUpdateFrameFacade::update(
    Tileset& tileset,
    const FrameState& frameState) {
    if (!frameState.camera) return;
    const double updateStartMs = perf::nowMs();

    // cesium-native: increment generation each frame so that
    // RenderCommand validator (non-zero check) accepts SurfaceTile commands.
    ++tileset.generation_;

    const TileFrameWorkResult frameWork =
        TileFrameWorkCoordinator::run(
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
                TilesetQueryFacade::maximumTransportActiveRequests(tileset),
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
                TilesetSelectionFrameFacade::refreshTilePlanRenderEntries(
                    tileset);
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

    const std::array<char, 384> updateDetail =
        TileFrameDebugLogFormatter::updateDetail(
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
                frameWork.resourceSmoothingActive});
    perf::logTimingAtLeast(frameState.frameId,
                           "Tileset.update",
                           perf::nowMs() - updateStartMs,
                           10.0,
                           updateDetail.data());
}

} // namespace earth_engine
