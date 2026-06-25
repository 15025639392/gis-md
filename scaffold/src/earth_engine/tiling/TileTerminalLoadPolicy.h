#pragma once

#include "TileLoadTypes.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"

namespace earth_engine {

class IPrepareRendererResources;
struct TilesetTile;

struct TileTerminalLoadAction {
    bool markEmptyCacheKey = false;
    bool ensureChildren = false;
    bool resourcesDirty = false;
};

struct TileTerminalLoadPolicy {
    static TileTerminalLoadAction applyTerminalResult(
        TileLoadDomain domain,
        TilesetTile& tile,
        TileLoadStatus status,
        IPrepareRendererResources* pPrepRenderer);
};

} // namespace earth_engine
