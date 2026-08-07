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
        return requestState_.callbacksDrained();
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
    result.gltfTerrainUploads = pendingLoads_.gltfTerrainUploadCount();
    result.gltfTerrainTerminalResults =
        pendingLoads_.gltfTerrainTerminalResultCount();
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

void TileLoadLifecycle::markRequestNeeded(const std::string& cacheKey) {
    std::lock_guard<std::mutex> lock(mutex_);
    requestState_.markNeeded(cacheKey);
}

size_t TileLoadLifecycle::sweepStaleRequests(uint64_t maxAgeFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<std::string> stale =
        requestState_.advanceFrameAndCollectStale(maxAgeFrames);
    for (const std::string& cacheKey : stale) {
        requestState_.cancelAndErase(cacheKey);
        pendingLoads_.eraseCacheKey(cacheKey);
    }
    return stale.size();
}

} // namespace earth_engine
