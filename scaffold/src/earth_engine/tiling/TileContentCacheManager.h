#pragma once

#include "LegacyHeightmapTerrainCacheMode.h"
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
        LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode);

    void markEligibleForUnloading(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        const std::string& cacheKey);
    void markIneligibleForUnloading(const std::string& cacheKey);

    void eraseTileIndexState(
        const std::string& cacheKey,
        TileContentLifecycleManager& lifecycle,
        TileLoadQueue& loadQueue,
        LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode =
            LegacyHeightmapTerrainCacheMode::Include);

    TileCacheUnloadContentResult unloadTileContent(
        TilesetTile& tile,
        TileContentLifecycleManager& lifecycle,
        IPrepareRendererResources* pPrepRenderer,
        LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode =
            LegacyHeightmapTerrainCacheMode::Include);

    template <typename ClearChildrenFn>
    void unloadCachedBytes(
        int64_t maximumCachedBytes,
        double unloadTimeLimitMs,
        bool resourceSmoothingActive,
        LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode,
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
            [this,
             &lifecycle,
             pPrepRenderer,
             legacyHeightmapCacheMode](TilesetTile& tile) {
                return unloadTileContent(
                    tile,
                    lifecycle,
                    pPrepRenderer,
                    legacyHeightmapCacheMode);
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
                legacyHeightmapCacheMode);
            cacheBytesDirty_ = false;
        }
    }

private:
    int64_t totalBytesUsed_ = 0;
    TileUnloadQueue unloadQueue_;
    bool cacheBytesDirty_ = true;
};

} // namespace earth_engine
