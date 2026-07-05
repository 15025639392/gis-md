#pragma once

#include "TilePlan.h"
#include "TilesetTile.h"
#include "TileSelectionSummaryPolicy.h"

#include <vector>

namespace earth_engine {

struct TileSelectionSummaryBuildInput {
    int selectedTilesCulled = 0;
    int selectedFogCulled = 0;
    int selectedTilesOccluded = 0;
    int selectedTilesWaitingForOcclusionResults = 0;
    int selectedCulledTilesVisited = 0;
};

struct TileSelectionSummaryBuildResult {
    int notYetRenderableCount = 0;
};

class TileSelectionSummaryBuilder {
public:
    template <typename IsTileRenderableFn>
    static TileSelectionSummaryBuildResult build(
        TilePlan& plan,
        const std::vector<TilesetTile*>& activeTiles,
        const TileSelectionSummaryBuildInput& input,
        IsTileRenderableFn&& isTileRenderable) {
        TileSelectionSummaryBuildResult result;

        // Iterate only the selection active-set (tiles visited this frame plus
        // decaying tiles) instead of the whole registry. The planTile()
        // aggregation below is filtered to `visited` (selectionState !=
        // NotVisited) and its per-tile contributions are commutative sums, so
        // iterating this subset in any order yields byte-identical results.
        for (TilesetTile* tile : activeTiles) {
            if (!tile) continue;
            const TileSelectionSummaryTilePlan summaryTilePlan =
                TileSelectionSummaryPolicy::planTile(
                    TileSelectionSummaryTileInput{
                        tile->key,
                        tile->selectionFrameState,
                        isTileRenderable(*tile)});
            if (!summaryTilePlan.visited) continue;

            plan.selectionRecords.push_back(summaryTilePlan.record);
            plan.selectionAncestorMeetsSseCount +=
                summaryTilePlan.selectionAncestorMeetsSseCount;
            plan.selectionKickedCount +=
                summaryTilePlan.selectionKickedCount;
            plan.selectionRenderedCount +=
                summaryTilePlan.selectionRenderedCount;
            plan.selectionRefinedCount +=
                summaryTilePlan.selectionRefinedCount;
            plan.cameraInsideNodeCount +=
                summaryTilePlan.cameraInsideNodeCount;
            plan.inFrustumNodeCount +=
                summaryTilePlan.inFrustumNodeCount;
            result.notYetRenderableCount +=
                summaryTilePlan.notYetRenderableCount;
        }

        const TileSelectionSummaryFramePlan summaryFramePlan =
            TileSelectionSummaryPolicy::planFrame(
                TileSelectionSummaryFrameInput{
                    plan.visibleTiles.size(),
                    plan.selectionRefinedCount,
                    input.selectedTilesCulled,
                    input.selectedFogCulled,
                    input.selectedTilesOccluded,
                    input.selectedTilesWaitingForOcclusionResults,
                    input.selectedCulledTilesVisited});
        plan.renderingNodeCount = summaryFramePlan.renderingNodeCount;
        plan.walkthroughNodeCount = summaryFramePlan.walkthroughNodeCount;
        plan.notRenderingNodeCount = summaryFramePlan.notRenderingNodeCount;
        plan.selectionOccludedCount =
            summaryFramePlan.selectionOccludedCount;
        plan.selectionWaitingForOcclusionResultsCount =
            summaryFramePlan.selectionWaitingForOcclusionResultsCount;
        plan.culledTilesVisitedCount =
            summaryFramePlan.culledTilesVisitedCount;
        plan.mercatorTileCount = summaryFramePlan.mercatorTileCount;

        return result;
    }
};

} // namespace earth_engine
