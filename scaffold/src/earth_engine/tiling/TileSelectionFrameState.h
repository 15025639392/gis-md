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
    // 无缝北极星机制 B(边吸附):4 条边各 3bit 的 log2 吸附步长,打包
    // W + 8·E + 64·N + 512·S(N=v0 边,S=v1 边;0=不吸附)。由
    // TileEdgeSnapResolver 每帧从**实际渲染集**算出——邻居比本瓦片粗 k 个
    // 八度(z 差 + dense/coarse 档差)时,本瓦片该边顶点在 shader 里吸附到
    // 2^k 间距的自纹理线性插值 → T-junction 在几何上不存在(残余只剩金字塔
    // 层间重采样差 ε,由裙墙覆盖)。逐帧重算天然覆盖"邻居本帧刚换档"暂态。
    float edgeSnapPacked = 0.0f;

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
