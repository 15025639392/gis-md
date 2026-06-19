#pragma once

#include "TileUpdateDebugLogInput.h"

namespace earth_engine {

struct FrameState;
class Tileset;

struct TilesetUpdateFrameRuntimeResult {
    TileUpdateDebugLogInput debugLog;
};

class TilesetUpdateFrameRuntime {
public:
    static TilesetUpdateFrameRuntimeResult run(Tileset& tileset,
                                               const FrameState& frameState);
};

} // namespace earth_engine
