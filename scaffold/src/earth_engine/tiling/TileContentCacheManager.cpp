#include "RasterMappedToTilesetTile.h"
#include "TileContentCacheManager.h"

namespace earth_engine {

void TileContentCacheManager::markResourcesDirty() {
    cacheBytesDirty_ = true;
}

void TileContentCacheManager::updateTotalBytesUsed(
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const TileContentLifecycleManager& lifecycle,
    LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode) {
    totalBytesUsed_ =
        legacyHeightmapCacheMode == LegacyHeightmapTerrainCacheMode::Include
        ? TileCacheMetrics::estimateTotalBytes(
              tiles,
              lifecycle.legacyTerrainCache())
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
    LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode) {
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        ignoredTerrainCache;
    auto& terrainCache =
        legacyHeightmapCacheMode == LegacyHeightmapTerrainCacheMode::Include
        ? lifecycle.legacyTerrainCache()
        : ignoredTerrainCache;
    TileIndexState::eraseCacheKeyState(
        cacheKey,
        unloadQueue_,
        terrainCache,
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
    LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode) {
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        ignoredTerrainCache;
    auto& terrainCache =
        legacyHeightmapCacheMode == LegacyHeightmapTerrainCacheMode::Include
        ? lifecycle.legacyTerrainCache()
        : ignoredTerrainCache;
    return TileContentUnloadCoordinator::unloadContent(
        tile,
        TileCacheKey::forTile(tile.key),
        terrainCache,
        lifecycle.emptyContentRegistry(),
        pPrepRenderer);
}

} // namespace earth_engine
