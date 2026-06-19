#pragma once

#include "../scene/Camera.h"
#include "TileFrameWorkCoordinator.h"

namespace earth_engine {

struct FrameState;
class Tileset;

class TilesetUpdateFrameRuntime {
public:
    static TileFrameWorkResult run(Tileset& tileset,
                                   const FrameState& frameState);
};

} // namespace earth_engine
