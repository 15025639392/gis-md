#include "RasterMappedToTilesetTile.h"
#include "TileContentLifecycleManager.h"

namespace earth_engine {

TileContentLifecycleManager::~TileContentLifecycleManager() {
    shutdown();
}

int TileContentLifecycleManager::pendingRequests() const {
    return loadLifecycle_.pendingRequestCount();
}

bool TileContentLifecycleManager::hasPendingWork() const {
    return loadLifecycle_.hasPendingWork();
}

void TileContentLifecycleManager::discardLegacyTerrainCacheForMode(
    LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode) {
    if (legacyHeightmapCacheMode ==
        LegacyHeightmapTerrainCacheMode::ContentOwnedTerrainOnly) {
        legacyTerrainCache_.clear();
    }
}

void TileContentLifecycleManager::shutdown() {
    loadLifecycle_.markDestroyingCancelAndWait();
}

} // namespace earth_engine
