#pragma once

#include "TileLoadQueue.h"
#include "TilePlan.h"
#include "TileSelectionCounters.h"
#include "TileSelectionPostTraversalPolicy.h"
#include "TileSelectionTraversalCounterPolicy.h"
#include "TileTraversalDetails.h"
#include "TilesetTile.h"

#include <cstddef>
#include <cstdint>

namespace earth_engine {

struct TileSelectionPostTraversalCommitContext {
    size_t firstRenderedDescendant = 0;
    size_t loadQueueBeforeChildren = 0;
    double tileSse = 0.0;
    double tilePriority = 0.0;
    bool renderable = false;
    // Children's aggregated notYetRenderableCount (pre-kick). cesium keeps this
    // count in the kicked tile's returned details unless the restore branch
    // resets it — see commit() / TilesetSelection.cpp:769.
    uint32_t childrenNotYetRenderableCount = 0;
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
                // cesium TilesetSelection.cpp:756: tilesKicked counts ONLY the
                // load-queue entries removed by the restore branch (descendant
                // limit exceeded) — an ordinary kick that trims descendants
                // from the render list does not increment it.
                selectionCounters.kicked += static_cast<int>(
                    loadQueue.size() - context.loadQueueBeforeChildren);
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
            TileTraversalDetails singleTileDetails =
                TileTraversalDetailsPolicy::forSingleTile(
                    context.renderable,
                    plan.wasReallyRenderedLastFrame);
            if (!plan.restoreChildLoadQueue) {
                // cesium TilesetSelection.cpp:769 resets notYetRenderableCount
                // to isRenderable?0:1 ONLY inside the restore branch. When
                // restore is not taken, the children's accumulated count keeps
                // propagating upward so an ancestor kick still sees how many
                // descendants are pending (forSingleTile already handled the
                // restore case's renderable?0:1).
                singleTileDetails.notYetRenderableCount =
                    context.childrenNotYetRenderableCount;
            }
            return TileSelectionPostTraversalCommitResult{
                true,
                singleTileDetails};
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
