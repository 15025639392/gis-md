#include "TileSelectionPlanAppender.h"

#include "RasterMappedToTilesetTile.h"
#include "TileLoadQueue.h"
#include "TilePlan.h"
#include "TileSelectionRenderEntryPolicy.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileSelectionPlanAppender::queueTileLoad(
    TileLoadQueue& loadQueue,
    const TileKey& key,
    TileLoadPriorityGroup group,
    double priority) {
    loadQueue.queue(key, group, priority);
}

void TileSelectionPlanAppender::addTileToCurrentPlan(
    TilePlan& tilePlan,
    TileLoadQueue& loadQueue,
    bool enableLodTransitionPeriod,
    TilesetTile& tile,
    double tileSse,
    bool queueForLoad,
    double tilePriority) {
    const TileSelectionRenderEntryPlan renderEntry =
        TileSelectionRenderEntryPolicy::plan(
            TileSelectionRenderEntryInput{
                enableLodTransitionPeriod,
                queueForLoad});
    if (renderEntry.writeSelectionState) {
        tile.selectionFrameState.selectionState = renderEntry.selectionState;
    }
    if (renderEntry.writeScreenSpaceError) {
        tile.selectionFrameState.screenSpaceError = tileSse;
    }
    if (renderEntry.resetLodTransitionFade) {
        tile.selectionFrameState.lodTransitionFadePercentage =
            renderEntry.lodTransitionFadeValue;
    }
    if (renderEntry.appendVisibleTile) {
        tilePlan.visibleTiles.push_back(tile.key);
    }
    if (renderEntry.queueNormalLoad) {
        queueTileLoad(
            loadQueue,
            tile.key,
            TileLoadPriorityGroup::Normal,
            tilePriority);
    }
}

} // namespace earth_engine
