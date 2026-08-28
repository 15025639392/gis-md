#include "RasterAssetDepot.h"

#include "RasterOverlayTileProvider.h"

namespace earth_engine {

RasterAssetAcquireResult RasterAssetDepot::acquireExactSource(
    RasterAssetConsumer consumer,
    RasterOverlayTileProvider& provider,
    const TileKey& sourceKey,
    std::function<bool()> tryAdmitTransport,
    std::function<void(RasterAssetResponse)> onReady) {
    RasterAssetAcquireResult result = provider.acquireExactSource(
        sourceKey,
        std::move(tryAdmitTransport),
        std::move(onReady));
    record(consumer, result.status);
    return result;
}

std::optional<RasterAssetSnapshot>
RasterAssetDepot::tryGetCachedExactSource(
    RasterAssetConsumer consumer,
    RasterOverlayTileProvider& provider,
    const TileKey& sourceKey) {
    std::optional<RasterAssetSnapshot> asset =
        provider.tryGetCachedExactSource(sourceKey);
    if (asset) {
        counters_[consumerIndex(consumer)].cacheHits.fetch_add(
            1, std::memory_order_relaxed);
    }
    return asset;
}

RasterAssetDepotStats RasterAssetDepot::stats(
    RasterAssetConsumer consumer) const {
    const Counters& counters = counters_[consumerIndex(consumer)];
    return RasterAssetDepotStats{
        counters.cacheHits.load(std::memory_order_relaxed),
        counters.joinedInFlight.load(std::memory_order_relaxed),
        counters.startedTransports.load(std::memory_order_relaxed),
        counters.admissionDenied.load(std::memory_order_relaxed)};
}

void RasterAssetDepot::record(
    RasterAssetConsumer consumer,
    RasterAssetAcquireStatus status) {
    Counters& counters = counters_[consumerIndex(consumer)];
    switch (status) {
        case RasterAssetAcquireStatus::CacheHit:
            counters.cacheHits.fetch_add(1, std::memory_order_relaxed);
            break;
        case RasterAssetAcquireStatus::JoinedInFlight:
            counters.joinedInFlight.fetch_add(1, std::memory_order_relaxed);
            break;
        case RasterAssetAcquireStatus::StartedTransport:
            counters.startedTransports.fetch_add(1, std::memory_order_relaxed);
            break;
        case RasterAssetAcquireStatus::AdmissionDenied:
            counters.admissionDenied.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

} // namespace earth_engine
