#pragma once

namespace earth_engine {

struct FrameState;
class Tileset;

class TilesetSelectionFrameFacade {
public:
    static void selectTiles(Tileset& tileset, const FrameState& frameState);
};

} // namespace earth_engine
