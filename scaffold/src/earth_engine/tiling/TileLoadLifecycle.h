#pragma once

#include "TilePendingLoadQueue.h"
#include "TilePendingRequestState.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace earth_engine {

struct TileLoadLifecycleCounts {
    PendingRequestCounts requests;
    size_t terrainUploads = 0;
    size_t terrainTerminalResults = 0;
    size_t contentUploads = 0;
    size_t contentTerminalResults = 0;
};

class TileLoadLifecycle {
public:
    std::mutex& mutex();
    std::mutex& mutex() const;
    std::condition_variable& condition();
    TilePendingRequestState& requestState();
    const TilePendingRequestState& requestState() const;
    TilePendingLoadQueue& pendingLoads();
    const TilePendingLoadQueue& pendingLoads() const;

    void markDestroyingCancelAndWait();
    int pendingRequestCount() const;
    TileLoadLifecycleCounts counts() const;
    bool hasPendingWork() const;
    bool containsWorkForCacheKey(const std::string& cacheKey) const;
    bool containsWorkForAnyCacheKey(
        const std::vector<std::string>& cacheKeys) const;
    void cancelAndEraseCacheKey(const std::string& cacheKey);

private:
    TilePendingRequestState requestState_;
    TilePendingLoadQueue pendingLoads_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};

} // namespace earth_engine
