#pragma once

#include "TileLoadTypes.h"
#include "TileTerminalLoadPolicy.h"

#include <string>

namespace earth_engine {

class TileEmptyContentRegistry;
struct TilesetTile;

struct TileTerminalLoadCommitter {
    static TileTerminalLoadAction commitTerrainTerminalResult(
        TilesetTile& tile,
        const std::string& cacheKey,
        TileLoadResult result,
        TileEmptyContentRegistry& emptyContentRegistry);
    static TileTerminalLoadAction commitContentTerminalResult(
        TilesetTile& tile,
        const std::string& cacheKey,
        TileLoadResult result,
        TileEmptyContentRegistry& emptyContentRegistry);
};

} // namespace earth_engine
