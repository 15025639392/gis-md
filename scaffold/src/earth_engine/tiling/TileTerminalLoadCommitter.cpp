#include "TileTerminalLoadCommitter.h"
#include "TileAvailabilityUpdateCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadResultMetadataApplicator.h"
#include <utility>
#include <vector>
namespace earth_engine {
namespace {
TileTerminalLoadAction commitTerminalResultImpl(
    TileLoadDomain domain,
    TilesetTile& tile,
    const std::string& cacheKey,
    TileLoadResult result,
    TileEmptyContentRegistry& emptyContentRegistry,
    IPrepareRendererResources* pPrepRenderer) {
    TileTerminalLoadAction action =
        TileTerminalLoadPolicy::applyTerminalResult(
            domain,
            tile,
            result.status,
            pPrepRenderer);
    if (domain == TileLoadDomain::TerrainContent) {
        if (result.shouldApplyTerminalMetadata() &&
            result.status == TileLoadStatus::Empty) {
            TileLoadResultMetadataApplicator::apply(
                tile,
                std::move(result.content.metadata));
        }
    } else if (result.shouldApplyTerminalMetadata()) {
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
} // namespace
TileTerminalLoadAction
TileTerminalLoadCommitter::commitTerminalResult(
    TileLoadDomain domain,
    TilesetTile& tile,
    const std::string& cacheKey,
    TileLoadResult result,
    TileEmptyContentRegistry& emptyContentRegistry,
    IPrepareRendererResources* pPrepRenderer,
    TilesetContentProvider* contentProvider) {
    std::vector<QuantizedMeshAvailabilityUpdate> terrainAvailabilityUpdates;
    terrainAvailabilityUpdates =
        TileAvailabilityUpdateCommitter::extractTerrainAvailabilityUpdates(
            domain,
            result,
            contentProvider);
    TileTerminalLoadAction action = commitTerminalResultImpl(
        domain,
        tile,
        cacheKey,
        std::move(result),
        emptyContentRegistry,
        pPrepRenderer);
    if (!terrainAvailabilityUpdates.empty()) {
        contentProvider->applyTerrainAvailabilityUpdates(
            terrainAvailabilityUpdates);
    }
    return action;
}
} // namespace earth_engine
