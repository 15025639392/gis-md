#include "RasterMappedToTilesetTile.h"
#include "TileContentCacheManager.h"

namespace earth_engine {

void TileContentCacheManager::markResourcesDirty() {
    cacheBytesDirty_ = true;
}

void TileContentCacheManager::updateTotalBytesUsed(
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileContentLifecycleManager& lifecycle,
    bool includeHeightmapTerrainCache) {
    totalBytesUsed_ =
        includeHeightmapTerrainCache
        ? TileCacheMetrics::estimateTotalBytes(
              tiles,
              lifecycle.heightmapTerrainCache())
        : TileCacheMetrics::estimateTotalBytes(tiles, {});
}

void TileContentCacheManager::markEligibleForUnloading(
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const std::string& cacheKey) {
    TileIndexState::markEligibleForUnloading(unloadQueue_, tiles, cacheKey);
}

void TileContentCacheManager::markIneligibleForUnloading(
    const std::string& cacheKey) {
    TileIndexState::markIneligibleForUnloading(unloadQueue_, cacheKey);
}

void TileContentCacheManager::eraseTileIndexState(
    const std::string& cacheKey,
    TileContentLifecycleManager& lifecycle,
    TileLoadQueue& loadQueue,
    bool includeHeightmapTerrainCache) {
    TileIndexState::eraseCacheKeyState(
        cacheKey,
        unloadQueue_,
        includeHeightmapTerrainCache
            ? &lifecycle.heightmapTerrainCache()
            : nullptr,
        lifecycle.emptyContentRegistry(),
        loadQueue,
        lifecycle.loadLifecycle(),
        [](const TileKey& tileKey) {
            return TileCacheKey::forTile(tileKey);
        });
}

TileCacheUnloadContentResult TileContentCacheManager::unloadTileContent(
    TilesetTile& tile,
    TileContentLifecycleManager& lifecycle,
    IPrepareRendererResources* pPrepRenderer,
    bool includeHeightmapTerrainCache) {
    return TileContentUnloadCoordinator::unloadContent(
        tile,
        TileCacheKey::forTile(tile.key),
        includeHeightmapTerrainCache
            ? &lifecycle.heightmapTerrainCache()
            : nullptr,
        lifecycle.emptyContentRegistry(),
        pPrepRenderer);
}

} // namespace earth_engine
