#include "TileTerminalLoadCommitter.h"
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
    if (domain == TileLoadDomain::TerrainContent &&
        contentProvider &&
        contentProvider->providesTerrainQuadtree() &&
        result.status == TileLoadStatus::Failed &&
        !result.quantizedMeshAvailabilityUpdates.empty()) {
        terrainAvailabilityUpdates =
            std::move(result.quantizedMeshAvailabilityUpdates);
    }
    TileTerminalLoadAction action = commitTerminalResultImpl(
        domain,
        tile,
        cacheKey,
        std::move(result),
        emptyContentRegistry,
        pPrepRenderer);
    if (!terrainAvailabilityUpdates.empty()) {
        // Availability updates must remain on the terrain path, but the
        // terminal policy itself is now shared.
        contentProvider->applyTerrainAvailabilityUpdates(
            terrainAvailabilityUpdates);
    }
    return action;
}
} // namespace earth_engine
