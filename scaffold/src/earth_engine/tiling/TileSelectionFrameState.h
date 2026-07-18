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
    double priority = 0.0;
    bool inFrustum = false;
    bool cameraInside = false;
    bool ancestorMeetsSse = false;
    float lodTransitionFadePercentage = 1.0f;
    // 距离连续 geomorph 进度(terrain only):由本瓦片 SSE 在有效 LOD 频带
    // (maxSSE/2, maxSSE] 内的位置决定,每帧刷新,喂给 geomorphUpFactor.w。
    // 1 = 全细节(无 morph/geomorph 关);0 = 粗起点≈父面(刚从父级细化出)。
    float terrainMorphFactor = 1.0f;

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
