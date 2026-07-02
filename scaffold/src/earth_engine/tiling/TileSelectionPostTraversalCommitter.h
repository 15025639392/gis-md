#pragma once

#include "TileLoadQueue.h"
#include "TilePlan.h"
#include "TileSelectionCounters.h"
#include "TileSelectionPostTraversalPolicy.h"
#include "TileSelectionTraversalCounterPolicy.h"
#include "TileTraversalDetails.h"
#include "TilesetTile.h"

#include <cstddef>

namespace earth_engine {

struct TileSelectionPostTraversalCommitContext {
    size_t firstRenderedDescendant = 0;
    size_t loadQueueBeforeChildren = 0;
    double tileSse = 0.0;
    double tilePriority = 0.0;
    bool renderable = false;
};

struct TileSelectionPostTraversalCommitResult {
    bool returnedSingleTileDetails = false;
    TileTraversalDetails details;
};

struct TileSelectionPostTraversalCommitter {
    template <typename KickDescendantsFn,
              typename QueueTileLoadFn,
              typename AddTileToCurrentPlanFn>
    static TileSelectionPostTraversalCommitResult commit(
        TilesetTile& tile,
        TilePlan& tilePlan,
        TileLoadQueue& loadQueue,
        TileSelectionCounters& selectionCounters,
        const TileSelectionPostTraversalCommitPlan& plan,
        const TileSelectionPostTraversalCommitContext& context,
        KickDescendantsFn&& kickDescendants,
        QueueTileLoadFn&& queueTileLoad,
        AddTileToCurrentPlanFn&& addTileToCurrentPlan) {
        if (plan.kickVisitedDescendants) {
            kickDescendants(tile);
        }
        if (plan.trimRenderedDescendants) {
            tilePlan.visibleTiles.erase(
                tilePlan.visibleTiles.begin() +
                    static_cast<std::ptrdiff_t>(
                        context.firstRenderedDescendant),
                tilePlan.visibleTiles.end());
        }
        const TileSelectionTraversalCounterPlan postCommitCounters =
            TileSelectionTraversalCounterPolicy::planPostTraversalCommit(
                plan);
        selectionCounters.kicked += postCommitCounters.kicked;

        if (plan.returnSingleTileDetails) {
            // cesium TilesetSelection.cpp:734-735: kickDescendantsAndRenderTile
            // sets the kicked tile's selection state to Rendered
            // UNCONDITIONALLY — even when the tile is not renderable or is
            // additive-refined. addTileToCurrentPlan (below) only runs for a
            // renderable non-additive replacement, so mark the state here to
            // keep next-frame wasRenderedLastFrame bookkeeping aligned.
            tile.selectionFrameState.selectionState =
                TileSelectionState::Rendered;
            if (plan.restoreChildLoadQueue) {
                loadQueue.resize(context.loadQueueBeforeChildren);
                if (plan.queueParentNormal) {
                    queueTileLoad(
                        tile.key,
                        TileLoadPriorityGroup::Normal,
                        context.tilePriority);
                }
            }

            if (plan.addRenderableReplacementToPlan) {
                addTileToCurrentPlan(
                    tile,
                    context.tileSse,
                    false,
                    context.tilePriority);
            }
            if (plan.queueParentPreload) {
                queueTileLoad(
                    tile.key,
                    TileLoadPriorityGroup::Preload,
                    context.tilePriority);
            }
            return TileSelectionPostTraversalCommitResult{
                true,
                TileTraversalDetailsPolicy::forSingleTile(
                    context.renderable,
                    plan.wasReallyRenderedLastFrame)};
        }

        if (plan.markTileRefined) {
            tile.selectionFrameState.selectionState =
                TileSelectionState::Refined;
            tile.selectionFrameState.screenSpaceError = context.tileSse;
        }
        if (plan.queueParentPreload) {
            queueTileLoad(
                tile.key,
                TileLoadPriorityGroup::Preload,
                context.tilePriority);
        }
        return TileSelectionPostTraversalCommitResult{};
    }
};

} // namespace earth_engine
