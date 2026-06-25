#pragma once

#include "TileLoadTypes.h"
#include "TileTerminalLoadPolicy.h"

#include <string>

namespace earth_engine {

class TileEmptyContentRegistry;
class IPrepareRendererResources;
class TilesetContentProvider;
struct TilesetTile;

struct TileTerminalLoadCommitter {
    static TileTerminalLoadAction commitTerrainTerminalResult(
        TilesetTile& tile,
        const std::string& cacheKey,
        TileLoadResult result,
        TileEmptyContentRegistry& emptyContentRegistry,
        IPrepareRendererResources* pPrepRenderer,
        TilesetContentProvider* contentProvider = nullptr);
    static TileTerminalLoadAction commitContentTerminalResult(
        TilesetTile& tile,
        const std::string& cacheKey,
        TileLoadResult result,
        TileEmptyContentRegistry& emptyContentRegistry,
        IPrepareRendererResources* pPrepRenderer);
};

} // namespace earth_engine
