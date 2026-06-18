#pragma once

#include "TileOcclusionState.h"
#include "TilePlan.h"

namespace earth_engine {

enum class TileSelectionOcclusionAction {
    None,
    StopForOccluded,
    StopForUnavailable
};

struct TileSelectionRefineDecision {
    bool refine = false;
    bool meetsSse = false;
};

struct TileSelectionContinueDeeperDecision {
    bool shouldContinue = false;
    bool ancestorMeetsSse = false;
    bool queueUrgent = false;
};

struct TileSelectionRefinementPolicy {
    static TileSelectionRefineDecision initialRefineDecision(
        bool unconditionallyRefine,
        bool meetsSse,
        bool ancestorMeetsSse);

    static bool shouldCheckOcclusion(
        bool enableOcclusionCulling,
        bool refine,
        bool unconditionallyRefine,
        TileSelectionState previousSelectionState,
        bool childWasRefinedLastFrame);

    static TileSelectionOcclusionAction occlusionAction(
        TileOcclusionState occlusion,
        bool delayRefinementForOcclusion,
        TileSelectionState previousSelectionState);

    static TileSelectionRefineDecision applyOcclusionAction(
        TileSelectionRefineDecision decision,
        TileSelectionOcclusionAction action);

    static TileSelectionContinueDeeperDecision continueDeeperDecision(
        bool refine,
        TileSelectionState previousSelectionState,
        bool renderable,
        bool ancestorMeetsSse);

    static bool shouldPreloadRefinedAncestor(
        bool preloadAncestors,
        bool queuedForLoad);
};

} // namespace earth_engine
