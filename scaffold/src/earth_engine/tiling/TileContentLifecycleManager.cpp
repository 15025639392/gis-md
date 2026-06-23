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

void TileContentLifecycleManager::discardLegacyTerrainCache(bool discard) {
    if (discard) {
        legacyTerrainCache_.clear();
    }
}

void TileContentLifecycleManager::shutdown() {
    loadLifecycle_.markDestroyingCancelAndWait();
}

} // namespace earth_engine
