#include "TileTerminalLoadCommitter.h"

#include "TileEmptyContentRegistry.h"
#include "TileLoadResultMetadataApplicator.h"

#include <utility>

namespace earth_engine {

TileTerminalLoadAction
TileTerminalLoadCommitter::commitTerrainTerminalResult(
    TilesetTile& tile,
    const std::string& cacheKey,
    TileLoadResult result,
    TileEmptyContentRegistry& emptyContentRegistry) {
    TileTerminalLoadAction action =
        TileTerminalLoadPolicy::applyTerrainTerminalResult(
            tile,
            result.status);
    if (result.shouldApplyTerminalMetadata()) {
        TileLoadResultMetadataApplicator::apply(
            tile,
            std::move(result.content.metadata));
    }
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
    TileLoadResult result,
    TileEmptyContentRegistry& emptyContentRegistry) {
    TileTerminalLoadAction action =
        TileTerminalLoadPolicy::applyContentTerminalResult(
            tile,
            result.status);
    if (result.shouldApplyTerminalMetadata()) {
        TileLoadResultMetadataApplicator::apply(
            tile,
            std::move(result.content.metadata));
    }
    if (action.markEmptyCacheKey) {
        emptyContentRegistry.insert(cacheKey);
    } else {
        emptyContentRegistry.erase(cacheKey);
    }
    return action;
}

} // namespace earth_engine
