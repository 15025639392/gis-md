#pragma once

#include "TilePlan.h"

namespace earth_engine {

struct TileSelectionRenderEntryInput {
    bool enableLodTransitionPeriod = true;
    bool queueForLoad = false;
};

struct TileSelectionRenderEntryPlan {
    TileSelectionState selectionState = TileSelectionState::Rendered;
    bool writeSelectionState = true;
    bool writeScreenSpaceError = true;
    bool resetLodTransitionFade = false;
    float lodTransitionFadeValue = 1.0f;
    bool appendVisibleTile = true;
    bool queueNormalLoad = false;
};

struct TileSelectionRenderEntryPolicy {
    static TileSelectionRenderEntryPlan plan(
        const TileSelectionRenderEntryInput& input);
};

} // namespace earth_engine
