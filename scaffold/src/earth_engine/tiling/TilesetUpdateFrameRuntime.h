#pragma once

#include "../scene/Camera.h"
#include "TileFrameDebugLogFormatter.h"
#include "TileFrameWorkCoordinator.h"

namespace earth_engine {

struct FrameState;
class Tileset;

struct TilesetUpdateFrameRuntimeResult {
    TileFrameWorkResult frameWork;
    TileUpdateDebugLogInput debugLog;
};

class TilesetUpdateFrameRuntime {
public:
    static TilesetUpdateFrameRuntimeResult run(Tileset& tileset,
                                               const FrameState& frameState);
};

} // namespace earth_engine
