#pragma once

#include "RasterAsset.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <utility>

namespace earth_engine {

class RasterOverlayTileProvider;

/// Identifies the runtime consumer of a shared decoded raster asset.
enum class RasterAssetConsumer {
    Direct = 0,
    PageStore = 1
};

struct RasterAssetDepotStats {
    uint64_t cacheHits = 0;
    uint64_t joinedInFlight = 0;
    uint64_t startedTransports = 0;
    uint64_t admissionDenied = 0;
};

/// Tileset-runtime access point for decoded raster source assets.
///
/// Storage, transport and waiter state remain in the provider's source depot,
/// because source mapping/fallback lifetime is provider-specific.  This class
/// owns the consumer-neutral access boundary: Direct and PageStore acquire the
/// same provider endpoint and therefore join one cache/in-flight request
/// without either backend depending on ImageryProvider transport APIs.
class RasterAssetDepot {
public:
    RasterAssetAcquireResult acquireExactSource(
        RasterAssetConsumer consumer,
        RasterOverlayTileProvider& provider,
        const TileKey& sourceKey,
        std::function<bool()> tryAdmitTransport,
        std::function<void(RasterAssetResponse)> onReady);

    std::optional<RasterAssetSnapshot> tryGetCachedExactSource(
        RasterAssetConsumer consumer,
        RasterOverlayTileProvider& provider,
        const TileKey& sourceKey);

    RasterAssetDepotStats stats(RasterAssetConsumer consumer) const;

private:
    struct Counters {
        std::atomic<uint64_t> cacheHits{0};
        std::atomic<uint64_t> joinedInFlight{0};
        std::atomic<uint64_t> startedTransports{0};
        std::atomic<uint64_t> admissionDenied{0};
    };

    static size_t consumerIndex(RasterAssetConsumer consumer) {
        return static_cast<size_t>(consumer);
    }
    void record(RasterAssetConsumer consumer, RasterAssetAcquireStatus status);

    std::array<Counters, 2> counters_;
};

} // namespace earth_engine
