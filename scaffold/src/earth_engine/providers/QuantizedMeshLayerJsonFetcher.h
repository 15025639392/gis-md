#pragma once

#include "TerrainProvider.h"

#include <functional>
#include <string>
#include <vector>

namespace earth_engine {

class PlatformBridge;

class QuantizedMeshLayerJsonFetcher {
public:
    using CancelPredicate = std::function<bool()>;

    explicit QuantizedMeshLayerJsonFetcher(PlatformBridge* platformBridge);

    std::vector<uint8_t> fetchBlocking(
        const std::string& url,
        HttpRequestPriority priority = HttpRequestPriority::Normal,
        CancelPredicate shouldCancel = {}) const;

private:
    PlatformBridge* platformBridge_ = nullptr;
};

} // namespace earth_engine
