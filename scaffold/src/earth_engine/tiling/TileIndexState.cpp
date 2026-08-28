#include "TileIndexState.h"

#include "DirectRasterMapping.h"
#include "TileEmptyContentRegistry.h"
#include "TileLoadLifecycle.h"
#include "TileLoadQueue.h"
#include "TileUnloadPolicy.h"
#include "TileUnloadQueue.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileIndexState::markEligibleForUnloading(
    TileUnloadQueue& unloadQueue,
    const TilesetTile* tile,
    const std::string& cacheKey) {
    if (!tile ||
        tile->referenceCount() > 0 ||
        !TileUnloadPolicy::isEligibleForContentUnloadQueue(*tile)) {
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
