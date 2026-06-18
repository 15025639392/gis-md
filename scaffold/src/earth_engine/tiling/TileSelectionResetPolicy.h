#pragma once

#include "TilePlan.h"

namespace earth_engine {

struct TileSelectionResetInput {
    TileSelectionState currentSelectionState = TileSelectionState::NotVisited;
    bool surfaceDrawable = false;
    bool completeRenderable = false;
};

struct TileSelectionResetPlan {
    TileSelectionState previousSelectionState = TileSelectionState::NotVisited;
    TileSelectionState selectionState = TileSelectionState::NotVisited;
    double screenSpaceError = 0.0;
    bool inFrustum = false;
    bool cameraInside = false;
    bool ancestorMeetsSse = false;
    bool surfaceDrawable = false;
    bool completeRenderable = false;
    bool renderable = false;
};

struct TileSelectionResetPolicy {
    static TileSelectionResetPlan plan(const TileSelectionResetInput& input);
};

} // namespace earth_engine
