#pragma once

#include "TileLoadState.h"

namespace earth_engine {

struct TileRenderableSnapshot {
    TileLoadState loadState = TileLoadState::Unloaded;
    TileContentKind contentKind = TileContentKind::Unknown;
    bool unconditionallyRefine = false;
    bool hasChildren = false;
    bool requiredRasterOverlaysReady = true;
    bool meshReady = false;
};

struct TileRenderablePolicy {
    static bool isCompleteRenderable(
        const TileRenderableSnapshot& snapshot);

    static bool hasSurfaceDrawable(
        TileContentKind contentKind,
        TileLoadState loadState,
        bool meshReady,
        bool hasGpuVertexBuffer);
};

} // namespace earth_engine
