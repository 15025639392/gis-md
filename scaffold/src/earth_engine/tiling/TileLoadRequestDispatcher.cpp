#include "TileLoadRequestDispatcher.h"

namespace earth_engine {

TileLoadDispatchResult TileLoadRequestDispatcher::queueUpsampledTerrain(
    std::mutex& mutex,
    TilePendingRequestState& requestState,
    TilePendingLoadQueue& pendingLoads,
    const TileKey& key,
    const std::string& cacheKey,
    TileLoadPriorityGroup group,
    double priority) {
    std::lock_guard<std::mutex> lock(mutex);
    if (requestState.destroying()) {
        return TileLoadDispatchResult::Destroying;
    }
    if (cacheKey.empty()) {
        return TileLoadDispatchResult::Skipped;
    }
    if (requestState.contains(cacheKey) ||
        pendingLoads.containsCacheKey(cacheKey)) {
        return TileLoadDispatchResult::Skipped;
    }
    pendingLoads.addTerrainUpload(
        PendingTerrainUpload{
            key,
            cacheKey,
            group,
            priority,
            TileLoadResult::createRenderable()});
    return TileLoadDispatchResult::Issued;
}

} // namespace earth_engine
