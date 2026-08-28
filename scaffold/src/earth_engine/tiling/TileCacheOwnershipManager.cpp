#include "TileCacheOwnershipManager.h"

#include "DirectRasterMapping.h"
#include "TileCacheKey.h"
#include "TileContentLifecycleManager.h"
#include "TilePendingUploadCompletion.h"
#include "GpuUploadQueue.h"
#include "TileLoadQueue.h"
#include "TileSubtreeRemovalCoordinator.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"
#include "../layers/ActivatedRasterOverlay.h"

#include <vector>

namespace earth_engine {

TileCacheOwnershipManager::TileCacheOwnershipManager(
    TileContentCacheManager& contentCache,
    TileContentLifecycleManager& contentLifecycle,
    TileLoadQueue& loadQueue,
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    bool& resourceSmoothingActiveForFrame,
    int64_t& maximumCachedBytes,
    double& tileCacheUnloadTimeLimit,
    GpuUploadQueue* gpuUploadQueue)
    : contentCache_(contentCache),
      contentLifecycle_(contentLifecycle),
      loadQueue_(loadQueue),
      tiles_(tiles),
      rasterOverlays_(rasterOverlays),
      resourceSmoothingActiveForFrame_(resourceSmoothingActiveForFrame),
      maximumCachedBytes_(maximumCachedBytes),
      tileCacheUnloadTimeLimit_(tileCacheUnloadTimeLimit),
      gpuUploadQueue_(gpuUploadQueue) {}

void TileCacheOwnershipManager::updateTotalBytesUsed() {
    contentCache_.updateTotalBytesUsed(tiles_, contentLifecycle_);
}

int64_t TileCacheOwnershipManager::totalBytesUsed() const {
    int64_t bytes = contentCache_.totalBytesUsed();
    for (const ActivatedRasterOverlay* overlay : rasterOverlays_) {
        if (overlay) {
            bytes += overlay->tileTextureBytesUsed();
        }
    }
    if (gpuUploadQueue_) {
        bytes += gpuUploadQueue_->pendingBytes();
    }
    return bytes;
}

int64_t TileCacheOwnershipManager::contentBytesUsed() const {
    return contentCache_.totalBytesUsed();
}

int64_t TileCacheOwnershipManager::imageryTextureBytesUsed() const {
    int64_t bytes = 0;
    for (const ActivatedRasterOverlay* overlay : rasterOverlays_) {
        if (overlay) {
            bytes += overlay->tileTextureBytesUsed();
        }
    }
    return bytes;
}

bool TileCacheOwnershipManager::shouldUnloadCachedBytes() const {
    return totalBytesUsed() > maximumCachedBytes_ ||
           contentCache_.hasPendingProtectedUnloads();
}

void TileCacheOwnershipManager::trimRasterCaches(bool cachePressure) {
    for (ActivatedRasterOverlay* overlay : rasterOverlays_) {
        if (overlay) {
            overlay->trimUnusedTiles(cachePressure);
        }
    }
}

void TileCacheOwnershipManager::markEligibleForUnloading(
    const TilesetTile* tile,
    const std::string& key) {
    contentCache_.markEligibleForUnloading(tile, key);
}

void TileCacheOwnershipManager::markIneligibleForUnloading(
    const std::string& key) {
    contentCache_.markIneligibleForUnloading(key);
}

void TileCacheOwnershipManager::eraseTileIndexState(const std::string& key) {
    if (gpuUploadQueue_) {
        gpuUploadQueue_->eraseCacheKey(key);
    }
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
    if (result != TileCacheUnloadContentResult::Keep && gpuUploadQueue_) {
        const std::string cacheKey = TileCacheKey::forTile(tile.key);
        gpuUploadQueue_->eraseCacheKey(cacheKey);
        TilePendingUploadCompletion::eraseUpload(
            contentLifecycle_.loadLifecycle(),
            cacheKey);
    }
    if (result == TileCacheUnloadContentResult::RemoveAndClearChildren) {
        clearChildrenRecursively(&tile, pPrepRenderer);
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
        tiles_,
        contentLifecycle_,
        pPrepRenderer,
        [this]() {
            return totalBytesUsed();
        },
        [this]() {
            trimRasterCaches(true);
        },
        [this, pPrepRenderer](TilesetTile& tile) {
            clearChildrenRecursively(&tile, pPrepRenderer);
        });
}

void TileCacheOwnershipManager::unloadConfiguredCachedBytes(
    IPrepareRendererResources* pPrepRenderer) {
    unloadCachedBytes(maximumCachedBytes_, pPrepRenderer);
}

} // namespace earth_engine
