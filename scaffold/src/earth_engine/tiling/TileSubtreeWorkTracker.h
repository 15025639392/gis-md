#pragma once

#include "TileLoadLifecycle.h"
#include "TileSubtreeTraversal.h"
#include "TilesetTile.h"

#include <string>
#include <vector>

namespace earth_engine {

class TileSubtreeWorkTracker {
public:
    template <typename CacheKeyForTileFn>
    static bool hasActiveContentWork(
        const TilesetTile& tile,
        const TileLoadLifecycle& loadLifecycle,
        CacheKeyForTileFn&& cacheKeyForTile) {
        const std::vector<std::string> keys =
            TileSubtreeTraversal::collectCacheKeys(
                tile,
                cacheKeyForTile);
        return loadLifecycle.containsWorkForAnyCacheKey(keys);
    }
};

} // namespace earth_engine
