#pragma once

#include "TileKey.h"
#include "TileSelectionFrameFinalizer.h"

namespace earth_engine {

struct FrameState;
struct TilesetTile;
struct TilesetTestAccess;
class Tileset;

class TilesetSelectionFrameFacade {
public:
    static void selectTiles(Tileset& tileset, const FrameState& frameState);
    static void refreshTilePlanRenderEntries(Tileset& tileset);

private:
    friend struct TilesetTestAccess;

    static TileSelectionFrameFinalizeTimings finalizeSelectedTilePlan(
        Tileset& tileset,
        const FrameState& frameState);
};

} // namespace earth_engine
