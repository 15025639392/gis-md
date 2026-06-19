#pragma once

#include "TileOcclusionCallback.h"

namespace earth_engine {

class Tileset;
struct TilesetTile;

class TilesetOcclusionFacade {
public:
    static void setOcclusionCallback(Tileset& tileset,
                                     TileOcclusionCallback callback);
    static void clearOcclusionCallback(Tileset& tileset);
    static TileOcclusionState checkOcclusion(const Tileset& tileset,
                                             const TilesetTile& tile);

private:
    static TileOcclusionState checkSingleTileOcclusion(
        const Tileset& tileset,
        const TilesetTile& tile);
};

} // namespace earth_engine
