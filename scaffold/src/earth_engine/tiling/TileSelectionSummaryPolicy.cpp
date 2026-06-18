#include "TileSelectionSummaryPolicy.h"

namespace earth_engine {

TileSelectionSummaryTilePlan TileSelectionSummaryPolicy::planTile(
    const TileSelectionSummaryTileInput& input) {
    TileSelectionSummaryTilePlan plan;
    if (input.selectionState == TileSelectionState::NotVisited) {
        return plan;
    }

    plan.visited = true;
    plan.record = TileSelectionRecord{
        input.key,
        input.selectionState,
        input.previousSelectionState,
        input.screenSpaceError,
        input.cameraInside,
        input.inFrustum,
        input.ancestorMeetsSse};

    if (input.ancestorMeetsSse) {
        plan.selectionAncestorMeetsSseCount = 1;
    }
    if (selectionWasKicked(input.selectionState)) {
        plan.selectionKickedCount = 1;
    }
    switch (input.selectionState) {
        case TileSelectionState::Rendered:
            plan.selectionRenderedCount = 1;
            break;
        case TileSelectionState::Refined:
            plan.selectionRefinedCount = 1;
            break;
        case TileSelectionState::RenderedAndKicked:
        case TileSelectionState::RefinedAndKicked:
        case TileSelectionState::Culled:
        case TileSelectionState::NotVisited:
            break;
    }
    if (input.cameraInside) {
        plan.cameraInsideNodeCount = 1;
    }
    if (input.inFrustum) {
        plan.inFrustumNodeCount = 1;
    }
    if (!input.renderable) {
        plan.notYetRenderableCount = 1;
    }

    return plan;
}

TileSelectionSummaryFramePlan TileSelectionSummaryPolicy::planFrame(
    const TileSelectionSummaryFrameInput& input) {
    TileSelectionSummaryFramePlan plan;
    plan.renderingNodeCount = static_cast<int>(input.visibleTileCount);
    plan.walkthroughNodeCount = input.selectionRefinedCount;
    plan.notRenderingNodeCount =
        input.selectedTilesCulled + input.selectedFogCulled;
    plan.selectionOccludedCount = input.selectedTilesOccluded;
    plan.selectionWaitingForOcclusionResultsCount =
        input.selectedTilesWaitingForOcclusionResults;
    plan.culledTilesVisitedCount = input.selectedCulledTilesVisited;
    plan.mercatorTileCount = static_cast<int>(input.visibleTileCount);
    return plan;
}

} // namespace earth_engine
