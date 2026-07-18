#pragma once

#include "../platform/bridge/PlatformBridge.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace earth_engine {

class PlatformBridge;

/// Generic blocking HTTP GET helper with HttpCache reuse and file:// support.
/// Used by scene setup paths (TMS/WMS/Bing metadata) that must fetch a document
/// synchronously before building an imagery provider.
class BlockingHttpFetcher {
public:
    using CancelPredicate = std::function<bool()>;

    explicit BlockingHttpFetcher(PlatformBridge* platformBridge);

    std::vector<uint8_t> fetchBlocking(
        const std::string& url,
        HttpRequestPriority priority = HttpRequestPriority::Normal,
        const std::vector<HttpRequestOptions::Header>& headers = {},
        CancelPredicate shouldCancel = {}) const;

private:
    PlatformBridge* platformBridge_ = nullptr;
};

} // namespace earth_engine
