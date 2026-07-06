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
        const TileContentLifecycleManager& lifecycle);

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
        // When resource smoothing is active and the cache is still over
        // budget, skip unloading tiles that are not already in the
        // Unloading state. This avoids a large single-frame spike in
        // freed GPU/CPU bytes. Tiles already mid-unload continue to
        // make progress.
        if (resourceSmoothingActive &&
            totalBytesUsed_ > maximumCachedBytes) {
            size_t candidates = unloadQueue_.size();
            while (candidates > 0 && !unloadQueue_.empty()) {
                --candidates;
                const std::string key = unloadQueue_.front();
                auto it = tiles.find(key);
                if (it == tiles.end()) {
                    unloadQueue_.popFront();
                    continue;
                }
                TilesetTile& tile = *it->second;
                if (tile.content.loadState == TileLoadState::Unloading) {
                    // Already mid-unload; let it continue through the
                    // normal coordinator path below.
                    break;
                }
                // Defer: remove from queue without unloading.
                unloadQueue_.popFront();
            }
            cacheBytesDirty_ = true;
            return;
        }
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
             pPrepRenderer](TilesetTile& tile) {
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
            updateTotalBytesUsed(tiles, lifecycle);
            cacheBytesDirty_ = false;
        }
    }

private:
    int64_t totalBytesUsed_ = 0;
    TileUnloadQueue unloadQueue_;
    bool cacheBytesDirty_ = true;
};

} // namespace earth_engine
