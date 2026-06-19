#include "TilesetUpdateFrameRuntime.h"

#include "TileContentAccess.h"
#include "TileContentLifecycleManager.h"
#include "Tileset.h"
#include "TilesetQueryFacade.h"
#include "TilesetSelectionFrameFacade.h"

namespace earth_engine {

namespace {

constexpr double kPostInteractionResourceSmoothingSeconds = 1.25;

} // namespace

TileFrameWorkResult TilesetUpdateFrameRuntime::run(
    Tileset& tileset,
    const FrameState& frameState) {
    return TileFrameWorkCoordinator::run(
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
}

} // namespace earth_engine
