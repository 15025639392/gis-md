#pragma once

#include "TileUpdateDebugLogInput.h"

namespace earth_engine {

struct FrameState;
class IPrepareRendererResources;
class Tileset;

struct TilesetUpdateFrameRuntimeResult {
    TileUpdateDebugLogInput debugLog;
};

class TilesetUpdateFrameRuntime {
public:
    static TilesetUpdateFrameRuntimeResult run(
        Tileset& tileset,
        const FrameState& frameState,
        IPrepareRendererResources* pPrepRenderer = nullptr);

private:
    /// 根层预载(漏底/黑块根修,pinBaseCoverage 开启时每帧调):种入全球
    /// z≤2 + 单独推进其影像。成员而非自由函数:需要 Tileset 的 friend 权限。
    static void runBaseCoveragePreload(
        Tileset& tileset,
        const FrameState& frameState,
        IPrepareRendererResources* pPrepRenderer);
};

} // namespace earth_engine
