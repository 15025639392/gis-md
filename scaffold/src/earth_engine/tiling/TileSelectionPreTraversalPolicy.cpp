#include "TileSelectionPreTraversalPolicy.h"

namespace earth_engine {

TileSelectionPreTraversalPlan TileSelectionPreTraversalPolicy::plan(
    const TileSelectionPreTraversalInput& input) {
    TileSelectionPreTraversalPlan plan;
    plan.queueUrgentLoad = input.refineFlow.queueUrgentLoad;
    plan.queuedForLoadAfterPreTraversal = input.refineFlow.queuedForLoad;
    plan.ancestorMeetsSseAfterPreTraversal =
        input.refineFlow.ancestorMeetsSse;

    if (!input.canRefine) {
        plan.finishAsSingleTile = true;
        plan.finishReason = TileSelectionPreTraversalFinishReason::CannotRefine;
        plan.singleTileShouldQueueLoad = true;
        return plan;
    }

    if (!input.refineFlow.refine) {
        plan.finishAsSingleTile = true;
        plan.finishReason =
            TileSelectionPreTraversalFinishReason::DoesNotRefine;
        plan.singleTileShouldQueueLoad =
            !input.refineFlow.ancestorMeetsSse;
        return plan;
    }

    plan.visitChildren = true;
    if (input.refineMode == TileRefine::Add) {
        plan.addAdditiveParentToPlan = true;
        plan.additiveParentShouldQueueLoad =
            !plan.queuedForLoadAfterPreTraversal;
        plan.queuedForLoadAfterPreTraversal = true;
    }

    return plan;
}

} // namespace earth_engine
