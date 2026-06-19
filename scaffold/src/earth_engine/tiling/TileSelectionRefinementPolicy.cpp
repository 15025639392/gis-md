#include "TileSelectionRefinementPolicy.h"

namespace earth_engine {

TileSelectionRefineDecision
TileSelectionRefinementPolicy::initialRefineDecision(
    bool unconditionallyRefine,
    bool meetsSse,
    bool ancestorMeetsSse) {
    return TileSelectionRefineDecision{
        unconditionallyRefine || (!meetsSse && !ancestorMeetsSse),
        meetsSse};
}

bool TileSelectionRefinementPolicy::shouldCheckOcclusion(
    bool enableOcclusionCulling,
    bool refine,
    bool unconditionallyRefine,
    TileSelectionState previousSelectionState,
    bool childWasRefinedLastFrame) {
    const bool tileLastRefined =
        previousSelectionState == TileSelectionState::Refined;
    return enableOcclusionCulling &&
           refine &&
           !unconditionallyRefine &&
           (!tileLastRefined || !childWasRefinedLastFrame);
}

TileSelectionOcclusionAction TileSelectionRefinementPolicy::occlusionAction(
    TileOcclusionState occlusion,
    bool delayRefinementForOcclusion,
    TileSelectionState previousSelectionState) {
    if (occlusion == TileOcclusionState::Occluded) {
        return TileSelectionOcclusionAction::StopForOccluded;
    }

    if (occlusion == TileOcclusionState::OcclusionUnavailable &&
        delayRefinementForOcclusion &&
        originalSelectionState(previousSelectionState) !=
            TileSelectionState::Refined) {
        return TileSelectionOcclusionAction::StopForUnavailable;
    }

    return TileSelectionOcclusionAction::None;
}

TileSelectionRefineDecision
TileSelectionRefinementPolicy::applyOcclusionAction(
    TileSelectionRefineDecision decision,
    TileSelectionOcclusionAction action) {
    if (action == TileSelectionOcclusionAction::None) {
        return decision;
    }

    decision.refine = false;
    decision.meetsSse = true;
    return decision;
}

TileSelectionContinueDeeperDecision
TileSelectionRefinementPolicy::continueDeeperDecision(
    bool refine,
    TileSelectionState previousSelectionState,
    bool renderable,
    bool ancestorMeetsSse) {
    if (refine ||
        originalSelectionState(previousSelectionState) !=
            TileSelectionState::Refined ||
        renderable) {
        return TileSelectionContinueDeeperDecision{};
    }

    return TileSelectionContinueDeeperDecision{
        true,
        true,
        !ancestorMeetsSse};
}

bool TileSelectionRefinementPolicy::shouldPreloadRefinedAncestor(
    bool preloadAncestors,
    bool queuedForLoad) {
    return preloadAncestors && !queuedForLoad;
}

} // namespace earth_engine
