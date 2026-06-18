#include "TileLoadRequestDispatcher.h"

#include "TileLoadPriorityPolicy.h"

namespace earth_engine {

TileLoadDispatchResult TileLoadRequestDispatcher::queueUpsampledTerrain(
    std::mutex& mutex,
    TilePendingRequestState& requestState,
    TilePendingLoadQueue& pendingLoads,
    FrameResourceBudget& budget,
    const TileKey& key,
    const std::string& cacheKey,
    TileLoadPriorityGroup group,
    double priority) {
    std::lock_guard<std::mutex> lock(mutex);
    if (requestState.destroying()) {
        return TileLoadDispatchResult::Destroying;
    }
    if (requestState.contains(cacheKey) ||
        pendingLoads.hasTerrainUpload(cacheKey)) {
        return TileLoadDispatchResult::Skipped;
    }
    if (!budget.tryIssue(FrameResourceLane::TerrainRequest,
                         TileLoadPriorityPolicy::toFramePriority(group))) {
        return TileLoadDispatchResult::Blocked;
    }
    pendingLoads.addTerrainUpload(
        PendingTerrainUpload{key, cacheKey, group, priority, nullptr});
    return TileLoadDispatchResult::Issued;
}

} // namespace earth_engine
