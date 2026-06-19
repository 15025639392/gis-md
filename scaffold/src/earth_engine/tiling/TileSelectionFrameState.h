#pragma once

#include "TilePlan.h"

namespace earth_engine {

struct TileSelectionFrameState {
    bool completeRenderable = false;
    bool renderable = false;
    bool subdivisionDesired = false;
    TileSelectionState previousSelectionState =
        TileSelectionState::NotVisited;
    TileSelectionState selectionState = TileSelectionState::NotVisited;
    double screenSpaceError = 0.0;
    bool inFrustum = false;
    bool cameraInside = false;
    bool ancestorMeetsSse = false;
    float lodTransitionFadePercentage = 1.0f;

    void updateFrameRenderability(bool complete) {
        completeRenderable = complete;
        renderable = complete;
    }

    void clearFrameRenderability() {
        completeRenderable = false;
        renderable = false;
    }

    void updateTraversalRenderability(bool traversalRenderable) {
        renderable = traversalRenderable;
    }
};

} // namespace earth_engine
