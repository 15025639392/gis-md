#pragma once

#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadQueue.h"
#include "TileLoadTypes.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace earth_engine {

struct DecodedHeightmap;
struct TileKey;
class TileLoadLifecycle;
class TileLoadQueue;
class TileEmptyContentRegistry;
class TileUnloadQueue;
struct TilesetTile;

struct TileIndexState {
    static void markEligibleForUnloading(
        TileUnloadQueue& unloadQueue,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        const std::string& cacheKey);

    static void markIneligibleForUnloading(
        TileUnloadQueue& unloadQueue,
        const std::string& cacheKey);

    template <typename CacheKeyForTileFn>
    static void eraseCacheKeyState(
        const std::string& cacheKey,
        TileUnloadQueue& unloadQueue,
        std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>* terrainCache,
        TileEmptyContentRegistry& emptyContentRegistry,
        TileLoadQueue& loadQueue,
        TileLoadLifecycle& loadLifecycle,
        CacheKeyForTileFn&& cacheKeyForTile) {
        markIneligibleForUnloading(unloadQueue, cacheKey);
        if (terrainCache) {
            terrainCache->erase(cacheKey);
        }
        emptyContentRegistry.erase(cacheKey);
        loadQueue.eraseIf([&](const TileLoadRequest& request) {
            return cacheKeyForTile(request.key) == cacheKey;
        });
        loadLifecycle.cancelAndEraseCacheKey(cacheKey);
    }
};

} // namespace earth_engine
