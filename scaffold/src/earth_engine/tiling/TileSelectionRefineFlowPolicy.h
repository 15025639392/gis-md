#pragma once

#include "TileOcclusionState.h"
#include "TilePlan.h"

#include <optional>

namespace earth_engine {

enum class TileSelectionRefineFlowCounter {
    None,
    Occluded,
    WaitingForOcclusion
};

struct TileSelectionRefineFlowOptions {
    bool enableOcclusionCulling = true;
    bool delayRefinementForOcclusion = true;
};

struct TileSelectionRefineFlowInput {
    bool unconditionallyRefine = false;
    bool meetsScreenSpaceError = false;
    bool ancestorMeetsSse = false;
    bool renderable = false;
    TileSelectionState previousSelectionState = TileSelectionState::NotVisited;
    bool childWasRefinedLastFrame = false;
    std::optional<TileOcclusionState> occlusion;
};

struct TileSelectionRefineFlowResult {
    bool refine = false;
    bool meetsScreenSpaceError = false;
    bool ancestorMeetsSse = false;
    bool shouldCheckOcclusion = false;
    bool queueUrgentLoad = false;
    bool queuedForLoad = false;
    TileSelectionRefineFlowCounter counter =
        TileSelectionRefineFlowCounter::None;
};

struct TileSelectionRefineFlowPolicy {
    static TileSelectionRefineFlowResult evaluate(
        const TileSelectionRefineFlowInput& input,
        const TileSelectionRefineFlowOptions& options);
};

} // namespace earth_engine
