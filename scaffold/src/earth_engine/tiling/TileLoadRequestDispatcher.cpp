#include "TileLoadRequestDispatcher.h"

#include <utility>

namespace earth_engine {

TileLoadDispatchResult TileLoadRequestDispatcher::queueUpsampledLoad(
    std::mutex& mutex,
    TilePendingRequestState& requestState,
    TilePendingLoadQueue& pendingLoads,
    const TileKey& key,
    const std::string& cacheKey,
    TileLoadPriorityGroup group,
    double priority,
    TileLoadDomain domain,
    TileLoadResult result) {
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
    result = TileLoadDomainPolicy::normalizeForDomain(
        domain,
        std::move(result));
    if (domain == TileLoadDomain::TerrainContent &&
        !result.isRenderableContentTerrain()) {
        pendingLoads.addTerminalResult(
            PendingTileLoad{
                domain,
                key,
                cacheKey,
                group,
                priority,
                std::move(result)});
        return TileLoadDispatchResult::Issued;
    }
    pendingLoads.addUpload(
        PendingTileLoad{
            domain,
            key,
            cacheKey,
            group,
            priority,
            std::move(result)});
    return TileLoadDispatchResult::Issued;
}

} // namespace earth_engine
