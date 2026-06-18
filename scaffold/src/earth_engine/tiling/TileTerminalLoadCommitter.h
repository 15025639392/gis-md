#pragma once

#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"
#include "TileTerminalLoadPolicy.h"

#include <string>

namespace earth_engine {

class TileEmptyContentRegistry;
struct TilesetTile;

struct TileTerminalLoadCommitter {
    static TileTerminalLoadAction commitTerrainTerminalResult(
        TilesetTile& tile,
        const std::string& cacheKey,
        TerrainTileLoadStatus status,
        TileEmptyContentRegistry& emptyContentRegistry);
    static TileTerminalLoadAction commitContentTerminalResult(
        TilesetTile& tile,
        const std::string& cacheKey,
        TileContentLoadStatus status,
        TileEmptyContentRegistry& emptyContentRegistry);
};

} // namespace earth_engine
