#include "TileTerminalLoadCommitter.h"

#include "TileEmptyContentRegistry.h"

namespace earth_engine {

TileTerminalLoadAction
TileTerminalLoadCommitter::commitTerrainTerminalResult(
    TilesetTile& tile,
    const std::string& cacheKey,
    TerrainTileLoadStatus status,
    TileEmptyContentRegistry& emptyContentRegistry) {
    TileTerminalLoadAction action =
        TileTerminalLoadPolicy::applyTerrainTerminalResult(tile, status);
    if (action.markEmptyCacheKey) {
        emptyContentRegistry.insert(cacheKey);
    } else {
        emptyContentRegistry.erase(cacheKey);
    }
    return action;
}

TileTerminalLoadAction
TileTerminalLoadCommitter::commitContentTerminalResult(
    TilesetTile& tile,
    const std::string& cacheKey,
    TileContentLoadStatus status,
    TileEmptyContentRegistry& emptyContentRegistry) {
    TileTerminalLoadAction action =
        TileTerminalLoadPolicy::applyContentTerminalResult(tile, status);
    if (action.markEmptyCacheKey) {
        emptyContentRegistry.insert(cacheKey);
    } else {
        emptyContentRegistry.erase(cacheKey);
    }
    return action;
}

} // namespace earth_engine
