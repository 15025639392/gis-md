#pragma once

#include "TileCacheUnloadCoordinator.h"
#include "TileCacheKey.h"
#include "TileContentLifecycleManager.h"
#include "TileContentUnloadCoordinator.h"
#include "TileIndexState.h"
#include "TileLoadQueue.h"
#include "TileSubtreeWorkTracker.h"
#include "TileUnloadQueue.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace earth_engine {

class IPrepareRendererResources;

class TileContentCacheManager {
public:
    int64_t totalBytesUsed() const { return totalBytesUsed_; }
    TileUnloadQueue& unloadQueue() { return unloadQueue_; }
    const TileUnloadQueue& unloadQueue() const { return unloadQueue_; }
    bool& cacheBytesDirty() { return cacheBytesDirty_; }
    bool cacheBytesDirty() const { return cacheBytesDirty_; }

    void markResourcesDirty();

    void updateTotalBytesUsed(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        const TileContentLifecycleManager& lifecycle,
        bool includeHeightmapTerrainCache);

    void markEligibleForUnloading(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        const std::string& cacheKey);
    void markIneligibleForUnloading(const std::string& cacheKey);

    void eraseTileIndexState(
        const std::string& cacheKey,
        TileContentLifecycleManager& lifecycle,
        TileLoadQueue& loadQueue);

    TileCacheUnloadContentResult unloadTileContent(
        TilesetTile& tile,
        TileContentLifecycleManager& lifecycle,
        IPrepareRendererResources* pPrepRenderer);

    template <typename ClearChildrenFn>
    void unloadCachedBytes(
        int64_t maximumCachedBytes,
        double unloadTimeLimitMs,
        bool resourceSmoothingActive,
        bool includeHeightmapTerrainCache,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        TileContentLifecycleManager& lifecycle,
        IPrepareRendererResources* pPrepRenderer,
        ClearChildrenFn&& clearChildren) {
        const TileCacheUnloadResult result = TileCacheUnloadCoordinator::run(
            unloadQueue_,
            tiles,
            totalBytesUsed_,
            maximumCachedBytes,
            unloadTimeLimitMs,
            resourceSmoothingActive,
            cacheBytesDirty_,
            [&lifecycle](const TilesetTile& tile) {
                return TileSubtreeWorkTracker::hasActiveContentWork(
                    tile,
                    lifecycle.loadLifecycle(),
                    [](const TileKey& key) {
                        return TileCacheKey::forTile(key);
                    });
            },
            [this, &lifecycle, pPrepRenderer](TilesetTile& tile) {
                return unloadTileContent(
                    tile,
                    lifecycle,
                    pPrepRenderer);
            },
            [this](const std::string& key) {
                markIneligibleForUnloading(key);
            },
            std::forward<ClearChildrenFn>(clearChildren));
        totalBytesUsed_ = result.totalBytesUsed;
        cacheBytesDirty_ = result.cacheBytesDirty;

        if (result.shouldRefreshTotalBytes) {
            updateTotalBytesUsed(
                tiles,
                lifecycle,
                includeHeightmapTerrainCache);
            cacheBytesDirty_ = false;
        }
    }

private:
    int64_t totalBytesUsed_ = 0;
    TileUnloadQueue unloadQueue_;
    bool cacheBytesDirty_ = true;
};

} // namespace earth_engine
