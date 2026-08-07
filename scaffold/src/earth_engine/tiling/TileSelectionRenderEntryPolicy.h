#pragma once

#include "TilePlan.h"

namespace earth_engine {

struct TileSelectionRenderEntryInput {
    bool queueForLoad = false;
};

struct TileSelectionRenderEntryPlan {
    TileSelectionState selectionState = TileSelectionState::Rendered;
    bool writeSelectionState = true;
    bool writeScreenSpaceError = true;
    bool appendVisibleTile = true;
    bool queueNormalLoad = false;
};

struct TileSelectionRenderEntryPolicy {
    static TileSelectionRenderEntryPlan plan(
        const TileSelectionRenderEntryInput& input);
};

} // namespace earth_engine
