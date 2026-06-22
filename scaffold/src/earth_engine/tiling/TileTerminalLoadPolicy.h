#pragma once

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
    static TileTerminalLoadAction applyTerrainTerminalResult(
        TilesetTile& tile,
        TileLoadStatus status,
        IPrepareRendererResources* pPrepRenderer);
    static TileTerminalLoadAction applyContentTerminalResult(
        TilesetTile& tile,
        TileLoadStatus status,
        IPrepareRendererResources* pPrepRenderer);
};

} // namespace earth_engine
