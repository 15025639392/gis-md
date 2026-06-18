#include "TileLoadLifecycle.h"

namespace earth_engine {

std::mutex& TileLoadLifecycle::mutex() {
    return mutex_;
}

std::mutex& TileLoadLifecycle::mutex() const {
    return mutex_;
}

std::condition_variable& TileLoadLifecycle::condition() {
    return condition_;
}

TilePendingRequestState& TileLoadLifecycle::requestState() {
    return requestState_;
}

const TilePendingRequestState& TileLoadLifecycle::requestState() const {
    return requestState_;
}

TilePendingLoadQueue& TileLoadLifecycle::pendingLoads() {
    return pendingLoads_;
}

const TilePendingLoadQueue& TileLoadLifecycle::pendingLoads() const {
    return pendingLoads_;
}

void TileLoadLifecycle::markDestroyingCancelAndWait() {
    std::unique_lock<std::mutex> lock(mutex_);
    requestState_.markDestroyingAndCancelRequests();
    pendingLoads_.clear();
    condition_.wait(lock, [this]() {
        return requestState_.empty();
    });
    requestState_.clearAfterCallbacksComplete();
}

int TileLoadLifecycle::pendingRequestCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(requestState_.totalRequestCount());
}

TileLoadLifecycleCounts TileLoadLifecycle::counts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    TileLoadLifecycleCounts result;
    result.requests = requestState_.counts();
    result.terrainUploads = pendingLoads_.terrainUploadCount();
    result.terrainTerminalResults =
        pendingLoads_.terrainTerminalResultCount();
    result.contentUploads = pendingLoads_.contentUploadCount();
    result.contentTerminalResults =
        pendingLoads_.contentTerminalResultCount();
    return result;
}

bool TileLoadLifecycle::hasPendingWork() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !requestState_.empty() || pendingLoads_.hasWork();
}

bool TileLoadLifecycle::containsWorkForCacheKey(
    const std::string& cacheKey) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return requestState_.contains(cacheKey) ||
           pendingLoads_.containsCacheKey(cacheKey);
}

bool TileLoadLifecycle::containsWorkForAnyCacheKey(
    const std::vector<std::string>& cacheKeys) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& cacheKey : cacheKeys) {
        if (requestState_.contains(cacheKey) ||
            pendingLoads_.containsCacheKey(cacheKey)) {
            return true;
        }
    }
    return false;
}

void TileLoadLifecycle::cancelAndEraseCacheKey(
    const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(mutex_);
    requestState_.cancelAndErase(cacheKey);
    pendingLoads_.eraseCacheKey(cacheKey);
}

} // namespace earth_engine
