#include "TileTerminalLoadCommitter.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadResultMetadataApplicator.h"
#include <utility>
namespace earth_engine {
namespace {
TileTerminalLoadAction commitTerrainTerminalResult(
    TilesetTile& tile,
    const std::string& cacheKey,
    TileLoadResult result,
    TileEmptyContentRegistry& emptyContentRegistry,
    IPrepareRendererResources* pPrepRenderer,
    TilesetContentProvider* contentProvider) {
    TileTerminalLoadAction action =
        TileTerminalLoadPolicy::applyTerminalResult(
        TileLoadDomain::TerrainContent,
            tile,
            result.status,
            pPrepRenderer);
    if (contentProvider &&
        contentProvider->providesTerrainQuadtree() &&
        result.status == TileLoadStatus::Failed &&
        !result.quantizedMeshAvailabilityUpdates.empty()) {
        contentProvider->applyTerrainAvailabilityUpdates(
            result.quantizedMeshAvailabilityUpdates);
    }
    if (result.shouldApplyTerminalMetadata() &&
        result.status == TileLoadStatus::Empty) {
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
TileTerminalLoadAction commitContentTerminalResult(
    TilesetTile& tile,
    const std::string& cacheKey,
    TileLoadResult result,
    TileEmptyContentRegistry& emptyContentRegistry,
    IPrepareRendererResources* pPrepRenderer) {
    TileTerminalLoadAction action =
        TileTerminalLoadPolicy::applyTerminalResult(
        TileLoadDomain::Content,
            tile,
            result.status,
            pPrepRenderer);
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
    if (domain == TileLoadDomain::TerrainContent) {
        return commitTerrainTerminalResult(
            tile,
            cacheKey,
            std::move(result),
            emptyContentRegistry,
            pPrepRenderer,
            contentProvider);
    }
    return commitContentTerminalResult(
        tile,
        cacheKey,
        std::move(result),
        emptyContentRegistry,
        pPrepRenderer);
}
} // namespace earth_engine