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
#include <unordered_set>
#include <utility>

namespace earth_engine {

class IPrepareRendererResources;

class TileContentCacheManager {
public:
    int64_t totalBytesUsed() const { return totalBytesUsed_; }
    TileUnloadQueue& unloadQueue() { return unloadQueue_; }
    const TileUnloadQueue& unloadQueue() const { return unloadQueue_; }
    bool hasPendingProtectedUnloads() const {
        return !pendingProtectedUnloads_.empty();
    }

    void updateTotalBytesUsed(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        const TileContentLifecycleManager& lifecycle);
    bool reconcileTileBytes(const TilesetTile& tile);
    void eraseTileBytes(const std::string& cacheKey);
    int64_t accountedTileBytes(const std::string& cacheKey) const;

    void markEligibleForUnloading(
        const TilesetTile* tile,
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

    template <typename CurrentTotalBytesFn,
              typename TrimExternalCachesFn,
              typename ClearChildrenFn>
    void unloadCachedBytes(
        int64_t maximumCachedBytes,
        double unloadTimeLimitMs,
        bool resourceSmoothingActive,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        TileContentLifecycleManager& lifecycle,
        IPrepareRendererResources* pPrepRenderer,
        CurrentTotalBytesFn&& currentTotalBytes,
        TrimExternalCachesFn&& trimExternalCaches,
        ClearChildrenFn&& clearChildren) {
        auto currentTotalBytesFn =
            std::forward<CurrentTotalBytesFn>(currentTotalBytes);
        const TileCacheUnloadResult result = TileCacheUnloadCoordinator::run(
            unloadQueue_,
            tiles,
            maximumCachedBytes,
            unloadTimeLimitMs,
            currentTotalBytesFn,
            [this,
             &currentTotalBytesFn](int64_t maximumBytes) {
                return currentTotalBytesFn() > maximumBytes ||
                       hasPendingProtectedUnloads();
            },
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
             pPrepRenderer](TilesetTile& tile) {
                return unloadTileContent(
                    tile,
                    lifecycle,
                    pPrepRenderer);
            },
            [this](const std::string& key) {
                markIneligibleForUnloading(key);
            },
            std::forward<ClearChildrenFn>(clearChildren),
            std::forward<TrimExternalCachesFn>(trimExternalCaches));
        (void)result;
        (void)resourceSmoothingActive;
    }

    template <typename ClearChildrenFn>
    void unloadCachedBytes(
        int64_t maximumCachedBytes,
        double unloadTimeLimitMs,
        bool resourceSmoothingActive,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        TileContentLifecycleManager& lifecycle,
        IPrepareRendererResources* pPrepRenderer,
        ClearChildrenFn&& clearChildren) {
        unloadCachedBytes(
            maximumCachedBytes,
            unloadTimeLimitMs,
            resourceSmoothingActive,
            tiles,
            lifecycle,
            pPrepRenderer,
            [this]() {
                return totalBytesUsed_;
            },
            []() {},
            std::forward<ClearChildrenFn>(clearChildren));
    }

private:
    struct TileByteAccount {
        uint64_t retainedResourcesRevision = 0;
        int64_t bytes = 0;
    };

    int64_t totalBytesUsed_ = 0;
    std::unordered_map<std::string, TileByteAccount> tileBytes_;
    TileUnloadQueue unloadQueue_;
    std::unordered_set<std::string> pendingProtectedUnloads_;
};

} // namespace earth_engine
