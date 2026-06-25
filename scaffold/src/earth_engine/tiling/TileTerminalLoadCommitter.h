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
    static TileTerminalLoadAction commitTerminalResult(
        TileLoadDomain domain,
        TilesetTile& tile,
        const std::string& cacheKey,
        TileLoadResult result,
        TileEmptyContentRegistry& emptyContentRegistry,
        IPrepareRendererResources* pPrepRenderer,
        TilesetContentProvider* contentProvider = nullptr);
};

} // namespace earth_engine
