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
    size_t gltfTerrainUploads = 0;
    size_t gltfTerrainTerminalResults = 0;
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

    /// 帧级"仍被需要"标记(锁内转发 TilePendingRequestState::markNeeded)。
    void markRequestNeeded(const std::string& cacheKey);
    /// 推进帧序,真取消连续 maxAgeFrames 帧无人再要的在飞请求,返回其数量。
    /// token.cancel 经 HttpRequestOptions.cancelFlag 桥接中止 curl 传输。
    size_t sweepStaleRequests(uint64_t maxAgeFrames);

private:
    TilePendingRequestState requestState_;
    TilePendingLoadQueue pendingLoads_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};

} // namespace earth_engine
