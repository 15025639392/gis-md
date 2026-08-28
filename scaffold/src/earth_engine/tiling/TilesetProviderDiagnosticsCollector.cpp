#include "TilesetProviderDiagnosticsCollector.h"

#include "../content/GltfContentProvider.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/ProviderRequestDiagnosticsAggregator.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../providers/TerrainProvider.h"

namespace earth_engine {

uint32_t TilesetProviderDiagnosticsSnapshot::maximumTransportActiveRequests(
    uint32_t fallback) const {
    if (allProviderRequests.maximumTransportActiveRequests < 0) {
        return fallback;
    }
    return static_cast<uint32_t>(
        allProviderRequests.maximumTransportActiveRequests);
}

void TilesetProviderDiagnosticsSnapshot::applyTo(
    TilesetLoadDiagnostics& diagnostics) const {
    diagnostics.terrainProviderRequests = terrainProviderRequests;
    diagnostics.contentProviderRequests = contentProviderRequests;
    diagnostics.rasterProviderRequests = rasterProviderRequests;
    diagnostics.rasterOverlayTilesLoading += rasterOverlayTilesLoading;
    diagnostics.rasterSourceRequestsInFlight += rasterSourceRequestsInFlight;
    diagnostics.rasterPendingUploads += rasterPendingUploads;
    diagnostics.rasterPendingUploadBytes += rasterPendingUploadBytes;
    diagnostics.peakRasterPendingUploadBytes += peakRasterPendingUploadBytes;
    diagnostics.rasterCachedSourceTileBytes += rasterCachedSourceTileBytes;
    diagnostics.peakRasterCachedSourceTileBytes +=
        peakRasterCachedSourceTileBytes;
}

TilesetProviderDiagnosticsSnapshot
TilesetProviderDiagnosticsCollector::collectContentAndRaster(
    const TilesetContentProvider* contentProvider,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    return collect(nullptr, contentProvider, rasterOverlays);
}

TilesetProviderDiagnosticsSnapshot
TilesetProviderDiagnosticsCollector::collect(
    const TerrainProvider* terrainProvider,
    const TilesetContentProvider* contentProvider,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    TilesetProviderDiagnosticsSnapshot snapshot;
    if (terrainProvider) {
        snapshot.terrainProviderRequests =
            terrainProvider->requestDiagnostics();
        ProviderRequestDiagnosticsAggregator::add(
            snapshot.allProviderRequests,
            snapshot.terrainProviderRequests);
    }
    if (contentProvider) {
        snapshot.contentProviderRequests =
            contentProvider->requestDiagnostics();
        ProviderRequestDiagnosticsAggregator::add(
            snapshot.allProviderRequests,
            snapshot.contentProviderRequests);
    }
    for (const ActivatedRasterOverlay* overlay : rasterOverlays) {
        if (!overlay || !overlay->getTileProvider()) {
            continue;
        }
        const RasterOverlayTileProvider* provider = overlay->getTileProvider();
        const ProviderRequestDiagnostics providerDiagnostics =
            provider->requestDiagnostics();
        ProviderRequestDiagnosticsAggregator::add(
            snapshot.rasterProviderRequests,
            providerDiagnostics);
        ProviderRequestDiagnosticsAggregator::add(
            snapshot.allProviderRequests,
            providerDiagnostics);
        snapshot.rasterOverlayTilesLoading +=
            provider->getThrottledTilesCurrentlyLoading();
        snapshot.rasterSourceRequestsInFlight +=
            provider->getActiveRasterSourceRequests();
        snapshot.rasterActiveDirectCompositeSourceSets +=
            provider->getActiveDirectCompositeSourceSetOrderCount();
        snapshot.rasterPendingSourceFallbacks +=
            provider->getPendingSourceFallbackCount();
        snapshot.rasterInFlightSourceTiles +=
            provider->getInFlightSourceTileCount();
        snapshot.rasterInFlightSourceWaiters +=
            provider->getInFlightSourceWaiterCount();
        snapshot.rasterPendingUploads += provider->getPendingUploadCount();
        const int64_t pendingUploadBytes =
            provider->getPendingUploadBudgetBytes();
        const int64_t peakPendingUploadBytes =
            provider->getPeakPendingUploadBudgetBytes();
        const int64_t cachedSourceTileBytes =
            provider->getCachedSourceTileBytes();
        snapshot.rasterPendingUploadBytes += pendingUploadBytes;
        snapshot.peakRasterPendingUploadBytes += peakPendingUploadBytes;
        snapshot.rasterCachedSourceTileBytes += cachedSourceTileBytes;
        snapshot.peakRasterCachedSourceTileBytes +=
            cachedSourceTileBytes;
    }
    return snapshot;
}

} // namespace earth_engine
