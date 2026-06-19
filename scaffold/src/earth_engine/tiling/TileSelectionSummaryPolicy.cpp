#include "TileSelectionSummaryPolicy.h"

namespace earth_engine {

TileSelectionSummaryTilePlan TileSelectionSummaryPolicy::planTile(
    const TileSelectionSummaryTileInput& input) {
    TileSelectionSummaryTilePlan plan;
    const TileSelectionFrameState& selection = input.selection;
    if (selection.selectionState == TileSelectionState::NotVisited) {
        return plan;
    }

    plan.visited = true;
    plan.record = TileSelectionRecord{
        input.key,
        selection.selectionState,
        selection.previousSelectionState,
        selection.screenSpaceError,
        selection.cameraInside,
        selection.inFrustum,
        selection.ancestorMeetsSse};

    if (selection.ancestorMeetsSse) {
        plan.selectionAncestorMeetsSseCount = 1;
    }
    if (selectionWasKicked(selection.selectionState)) {
        plan.selectionKickedCount = 1;
    }
    switch (selection.selectionState) {
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
    if (selection.cameraInside) {
        plan.cameraInsideNodeCount = 1;
    }
    if (selection.inFrustum) {
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
