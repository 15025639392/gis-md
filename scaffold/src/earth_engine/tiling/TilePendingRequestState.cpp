#include "TilePendingRequestState.h"

namespace earth_engine {

bool TilePendingRequestState::destroying() const {
    return destroying_;
}

bool TilePendingRequestState::empty() const {
    return pendingRequests_.empty();
}

bool TilePendingRequestState::contains(const std::string& cacheKey) const {
    return pendingRequests_.count(cacheKey) != 0;
}

size_t TilePendingRequestState::totalRequestCount() const {
    return pendingRequests_.size();
}

PendingRequestCounts TilePendingRequestState::counts() const {
    PendingRequestCounts result;
    result.totalRequests = pendingRequests_.size();
    result.contentRequests = pendingContentRequestKeys_.size();
    result.terrainRequests =
        result.totalRequests > result.contentRequests
            ? result.totalRequests - result.contentRequests
            : 0;
    return result;
}

bool TilePendingRequestState::beginTerrainRequest(
    const std::string& cacheKey,
    const CancellationToken& token) {
    if (destroying_ || pendingRequests_.count(cacheKey)) {
        return false;
    }
    pendingRequests_.insert(cacheKey);
    pendingRequestTokens_[cacheKey] = token;
    return true;
}

bool TilePendingRequestState::beginContentRequest(
    const std::string& cacheKey,
    const CancellationToken& token) {
    if (destroying_ || pendingRequests_.count(cacheKey)) {
        return false;
    }
    pendingRequests_.insert(cacheKey);
    pendingContentRequestKeys_.insert(cacheKey);
    pendingRequestTokens_[cacheKey] = token;
    return true;
}

void TilePendingRequestState::completeTerrainRequest(
    const std::string& cacheKey) {
    pendingRequests_.erase(cacheKey);
    pendingRequestTokens_.erase(cacheKey);
}

void TilePendingRequestState::completeContentRequest(
    const std::string& cacheKey) {
    pendingRequests_.erase(cacheKey);
    pendingContentRequestKeys_.erase(cacheKey);
    pendingRequestTokens_.erase(cacheKey);
}

void TilePendingRequestState::cancelAndErase(const std::string& cacheKey) {
    if (auto tokenIt = pendingRequestTokens_.find(cacheKey);
        tokenIt != pendingRequestTokens_.end()) {
        tokenIt->second.cancel();
    }
    pendingRequests_.erase(cacheKey);
    pendingContentRequestKeys_.erase(cacheKey);
    pendingRequestTokens_.erase(cacheKey);
}

void TilePendingRequestState::markDestroyingAndCancelRequests() {
    destroying_ = true;
    for (auto& [cacheKey, token] : pendingRequestTokens_) {
        token.cancel();
        (void)cacheKey;
    }
    pendingContentRequestKeys_.clear();
}

void TilePendingRequestState::clearAfterCallbacksComplete() {
    pendingRequestTokens_.clear();
}

} // namespace earth_engine
