#include "TileIndexState.h"

#include "RasterMappedToTilesetTile.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadQueue.h"
#include "TileUnloadPolicy.h"
#include "TileUnloadQueue.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileIndexState::markEligibleForUnloading(
    TileUnloadQueue& unloadQueue,
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const std::string& cacheKey) {
    const auto tileIt = tiles.find(cacheKey);
    if (tileIt == tiles.end() ||
        !tileIt->second ||
        !TileUnloadPolicy::isEligibleForContentUnloadQueue(*tileIt->second)) {
        return;
    }
    unloadQueue.pushBackIfAbsent(cacheKey);
}

void TileIndexState::markIneligibleForUnloading(
    TileUnloadQueue& unloadQueue,
    const std::string& cacheKey) {
    unloadQueue.erase(cacheKey);
}

} // namespace earth_engine
