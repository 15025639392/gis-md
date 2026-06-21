#pragma once

#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"

namespace earth_engine {

struct TilesetTile;

struct TileTerminalLoadAction {
    bool markEmptyCacheKey = false;
    bool ensureChildren = false;
    bool resourcesDirty = false;
};

struct TileTerminalLoadPolicy {
    static TileTerminalLoadAction applyTerrainTerminalResult(
        TilesetTile& tile,
        TileLoadStatus status);
    static TileTerminalLoadAction applyContentTerminalResult(
        TilesetTile& tile,
        TileLoadStatus status);
};

} // namespace earth_engine
