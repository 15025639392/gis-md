#include "TileCacheOwnershipManager.h"

#include "RasterMappedToTilesetTile.h"
#include "TileCacheKey.h"
#include "TileContentLifecycleManager.h"
#include "TileLoadQueue.h"
#include "TileSubtreeRemovalCoordinator.h"
#include "TilesetTile.h"

namespace earth_engine {

TileCacheOwnershipManager::TileCacheOwnershipManager(
    TileContentCacheManager& contentCache,
    TileContentLifecycleManager& contentLifecycle,
    TileLoadQueue& loadQueue,
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    bool& resourceSmoothingActiveForFrame,
    int64_t& maximumCachedBytes,
    double& tileCacheUnloadTimeLimit,
    bool includeHeightmapTerrainCache)
    : contentCache_(contentCache),
      contentLifecycle_(contentLifecycle),
      loadQueue_(loadQueue),
      tiles_(tiles),
      resourceSmoothingActiveForFrame_(resourceSmoothingActiveForFrame),
      maximumCachedBytes_(maximumCachedBytes),
      tileCacheUnloadTimeLimit_(tileCacheUnloadTimeLimit),
      includeHeightmapTerrainCache_(includeHeightmapTerrainCache) {}

void TileCacheOwnershipManager::updateTotalBytesUsed() {
    if (!includeHeightmapTerrainCache_) {
        contentLifecycle_.heightmapTerrainCache().clear();
    }
    contentCache_.updateTotalBytesUsed(
        tiles_,
        contentLifecycle_,
        includeHeightmapTerrainCache_);
}

void TileCacheOwnershipManager::markEligibleForUnloading(
    const std::string& key) {
    contentCache_.markEligibleForUnloading(tiles_, key);
}

void TileCacheOwnershipManager::markIneligibleForUnloading(
    const std::string& key) {
    contentCache_.markIneligibleForUnloading(key);
}

void TileCacheOwnershipManager::eraseTileIndexState(const std::string& key) {
    contentCache_.eraseTileIndexState(
        key,
        contentLifecycle_,
        loadQueue_);
}

void TileCacheOwnershipManager::clearChildrenRecursively(
    TilesetTile* tile,
    IPrepareRendererResources* pPrepRenderer) {
    TileSubtreeRemovalCoordinator::clearChildrenRecursively(
        tile,
        tiles_,
        pPrepRenderer,
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [this](const std::string& cacheKey) {
            eraseTileIndexState(cacheKey);
        });
}

TileCacheUnloadContentResult TileCacheOwnershipManager::unloadTileContent(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    const TileCacheUnloadContentResult result = contentCache_.unloadTileContent(
        tile,
        contentLifecycle_,
        pPrepRenderer);
    if (result == TileCacheUnloadContentResult::RemoveAndClearChildren) {
        clearChildrenRecursively(&tile, pPrepRenderer);
    }
    if (result != TileCacheUnloadContentResult::Keep) {
        contentCache_.markResourcesDirty();
    }
    return result;
}

void TileCacheOwnershipManager::unloadCachedBytes(
    int64_t maximumCachedBytes,
    IPrepareRendererResources* pPrepRenderer) {
    contentCache_.unloadCachedBytes(
        maximumCachedBytes,
        tileCacheUnloadTimeLimit_,
        resourceSmoothingActiveForFrame_,
        includeHeightmapTerrainCache_,
        tiles_,
        contentLifecycle_,
        pPrepRenderer,
        [this, pPrepRenderer](TilesetTile& tile) {
            clearChildrenRecursively(&tile, pPrepRenderer);
        });
}

void TileCacheOwnershipManager::unloadConfiguredCachedBytes(
    IPrepareRendererResources* pPrepRenderer) {
    unloadCachedBytes(maximumCachedBytes_, pPrepRenderer);
}

} // namespace earth_engine
