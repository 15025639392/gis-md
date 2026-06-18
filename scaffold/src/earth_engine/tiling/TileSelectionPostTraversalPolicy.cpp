#include "TileSelectionPostTraversalPolicy.h"

#include "TileSelectionRefinementPolicy.h"

namespace earth_engine {

TileSelectionPostTraversalResult TileSelectionPostTraversalPolicy::evaluate(
    const TileSelectionPostTraversalInput& input,
    const TileSelectionPostTraversalOptions& options) {
    TileSelectionPostTraversalResult result;
    result.shouldKick = TileSelectionKickPolicy::shouldKickDescendants(
        input.traversalDetails,
        input.renderable,
        input.unconditionallyRefine,
        options.loadingDescendantLimit,
        options.enableLodTransitionPeriod,
        options.kickDescendantsWhileFadingIn,
        input.previousSelectionState,
        input.hasLodTransitionRenderContent,
        input.lodTransitionFadePercentage);
    result.wasReallyRenderedLastFrame =
        input.wasRenderedLastFrame && input.renderable;

    if (result.shouldKick) {
        result.kickPlan = TileSelectionKickPolicy::planAfterKick(
            input.traversalDetails,
            result.wasReallyRenderedLastFrame,
            options.loadingDescendantLimit,
            input.isExternalContent,
            input.unconditionallyRefine,
            input.refineMode,
            input.renderable,
            input.queuedForLoad);
        return result;
    }

    result.preloadRefinedAncestor =
        TileSelectionRefinementPolicy::shouldPreloadRefinedAncestor(
            options.preloadAncestors,
            input.queuedForLoad);
    return result;
}

TileSelectionPostTraversalCommitPlan
TileSelectionPostTraversalPolicy::commitPlan(
    const TileSelectionPostTraversalResult& result,
    bool queuedForLoad) {
    TileSelectionPostTraversalCommitPlan plan;
    plan.wasReallyRenderedLastFrame = result.wasReallyRenderedLastFrame;

    if (result.shouldKick) {
        plan.kickVisitedDescendants = true;
        plan.trimRenderedDescendants = true;
        plan.returnSingleTileDetails = true;
        plan.restoreChildLoadQueue =
            result.kickPlan.restoreChildLoadQueueAndLoadParent;
        plan.queueParentNormal =
            result.kickPlan.restoreChildLoadQueueAndLoadParent &&
            !queuedForLoad;
        plan.addRenderableReplacementToPlan =
            result.kickPlan.addRenderableReplacementToPlan;
        plan.queueParentPreload = result.kickPlan.preloadParent;
        return plan;
    }

    plan.markTileRefined = true;
    plan.queueParentPreload = result.preloadRefinedAncestor;
    return plan;
}

} // namespace earth_engine
