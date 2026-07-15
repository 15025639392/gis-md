#pragma once

#include "../threading/CancellationToken.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace earth_engine {

struct PendingRequestCounts {
    size_t terrainRequests = 0;
    size_t contentRequests = 0;
    size_t totalRequests = 0;
};

class TilePendingRequestState {
public:
    bool destroying() const;
    bool empty() const;
    bool callbacksDrained() const;
    bool contains(const std::string& cacheKey) const;

    size_t totalRequestCount() const;
    PendingRequestCounts counts() const;

    bool beginTerrainRequest(const std::string& cacheKey,
                             const CancellationToken& token);
    bool beginContentRequest(const std::string& cacheKey,
                             const CancellationToken& token);

    void completeTerrainRequest(const std::string& cacheKey);
    void completeContentRequest(const std::string& cacheKey);
    void completeTerrainRequest(
        const std::string& cacheKey,
        const CancellationToken& token);
    void completeContentRequest(
        const std::string& cacheKey,
        const CancellationToken& token);
    void cancelAndErase(const std::string& cacheKey);

    void markDestroyingAndCancelRequests();
    void clearAfterCallbacksComplete();

private:
    std::unordered_set<std::string> pendingRequests_;
    std::unordered_set<std::string> pendingContentRequestKeys_;
    std::unordered_map<std::string, CancellationToken> pendingRequestTokens_;
    std::vector<CancellationToken> retiredCallbackTokens_;
    bool destroying_ = false;
};

} // namespace earth_engine
