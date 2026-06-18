#include "TileSelectionRefineFlowPolicy.h"

#include "TileSelectionRefinementPolicy.h"

namespace earth_engine {

TileSelectionRefineFlowResult TileSelectionRefineFlowPolicy::evaluate(
    const TileSelectionRefineFlowInput& input,
    const TileSelectionRefineFlowOptions& options) {
    TileSelectionRefineFlowResult result;
    result.ancestorMeetsSse = input.ancestorMeetsSse;

    TileSelectionRefineDecision refineDecision =
        TileSelectionRefinementPolicy::initialRefineDecision(
            input.unconditionallyRefine,
            input.meetsScreenSpaceError,
            input.ancestorMeetsSse);
    result.refine = refineDecision.refine;
    result.meetsScreenSpaceError = refineDecision.meetsSse;

    result.shouldCheckOcclusion =
        TileSelectionRefinementPolicy::shouldCheckOcclusion(
            options.enableOcclusionCulling,
            result.refine,
            input.unconditionallyRefine,
            input.previousSelectionState,
            input.childWasRefinedLastFrame);
    if (result.shouldCheckOcclusion && input.occlusion) {
        const TileSelectionOcclusionAction occlusionAction =
            TileSelectionRefinementPolicy::occlusionAction(
                *input.occlusion,
                options.delayRefinementForOcclusion,
                input.previousSelectionState);
        if (occlusionAction == TileSelectionOcclusionAction::StopForOccluded) {
            result.counter = TileSelectionRefineFlowCounter::Occluded;
        } else if (
            occlusionAction ==
            TileSelectionOcclusionAction::StopForUnavailable) {
            result.counter =
                TileSelectionRefineFlowCounter::WaitingForOcclusion;
        }

        refineDecision = TileSelectionRefinementPolicy::applyOcclusionAction(
            TileSelectionRefineDecision{
                result.refine,
                result.meetsScreenSpaceError},
            occlusionAction);
        result.refine = refineDecision.refine;
        result.meetsScreenSpaceError = refineDecision.meetsSse;
    }

    const TileSelectionContinueDeeperDecision continueDeeper =
        TileSelectionRefinementPolicy::continueDeeperDecision(
            result.refine,
            input.previousSelectionState,
            input.renderable,
            result.ancestorMeetsSse);
    if (continueDeeper.shouldContinue) {
        result.refine = true;
        result.ancestorMeetsSse = continueDeeper.ancestorMeetsSse;
        result.queueUrgentLoad = continueDeeper.queueUrgent;
        result.queuedForLoad = continueDeeper.queueUrgent;
    }

    return result;
}

} // namespace earth_engine
