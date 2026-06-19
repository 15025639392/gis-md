#pragma once

namespace earth_engine {

struct FrameState;
class Tileset;

class TilesetUpdateFrameFacade {
public:
    static void update(Tileset& tileset, const FrameState& frameState);
};

} // namespace earth_engine
