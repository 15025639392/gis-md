#include "TilesetQueryFacade.h"

#include "LoadedTerrainHeightSampler.h"
#include "TileContentCacheManager.h"
#include "TileContentLifecycleManager.h"
#include "TileLoadQueue.h"
#include "Tileset.h"
#include "TilesetTileRegistry.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "TileFrameResourceBudgetPlanner.h"

#include <algorithm>

namespace earth_engine {

namespace {

void addProviderRequestDiagnostics(ProviderRequestDiagnostics& total,
                                   const ProviderRequestDiagnostics& next) {
    total.requestsStarted += next.requestsStarted;
    total.requestsCompleted += next.requestsCompleted;
    total.activeWorkerBlockingRequests +=
        next.activeWorkerBlockingRequests;
    total.peakWorkerBlockingRequests =
        std::max(total.peakWorkerBlockingRequests,
                 next.peakWorkerBlockingRequests);
    total.externalResourceRequestsStarted +=
        next.externalResourceRequestsStarted;
    total.externalResourceRequestsCompleted +=
        next.externalResourceRequestsCompleted;
    total.activeExternalResourceBlockingRequests +=
        next.activeExternalResourceBlockingRequests;
    total.peakExternalResourceBlockingRequests =
        std::max(total.peakExternalResourceBlockingRequests,
                 next.peakExternalResourceBlockingRequests);
    if (next.maximumTransportActiveRequests >= 0) {
        total.maximumTransportActiveRequests =
            std::max(total.maximumTransportActiveRequests,
                     next.maximumTransportActiveRequests);
    }
}

} // namespace

int TilesetQueryFacade::cachedTerrainTiles(const Tileset& tileset) {
    return static_cast<int>(tileset.contentLifecycle_.terrainCache().size());
}

int TilesetQueryFacade::pendingRequests(const Tileset& tileset) {
    return tileset.contentLifecycle_.pendingRequests();
}

int64_t TilesetQueryFacade::totalBytesUsed(const Tileset& tileset) {
    return tileset.contentCache_.totalBytesUsed();
}

uint32_t TilesetQueryFacade::maximumTransportActiveRequests(
    const Tileset& tileset) {
    ProviderRequestDiagnostics diagnostics;
    if (tileset.terrainProvider_) {
        addProviderRequestDiagnostics(
            diagnostics,
            tileset.terrainProvider_->requestDiagnostics());
    }
    if (tileset.contentProvider_) {
        addProviderRequestDiagnostics(
            diagnostics,
            tileset.contentProvider_->requestDiagnostics());
    }
    for (const ActivatedRasterOverlay* overlay : tileset.rasterOverlays_) {
        if (!overlay || !overlay->getTileProvider()) {
            continue;
        }
        addProviderRequestDiagnostics(
            diagnostics,
            overlay->getTileProvider()->requestDiagnostics());
    }
    if (diagnostics.maximumTransportActiveRequests < 0) {
        return TileFrameResourceBudgetPlanInput::
            kDefaultMaximumTransportActiveRequests;
    }
    return static_cast<uint32_t>(
        diagnostics.maximumTransportActiveRequests);
}

TilesetLoadDiagnostics TilesetQueryFacade::loadDiagnostics(
    const Tileset& tileset) {
    TilesetLoadDiagnostics diagnostics = TileLoadDiagnosticsCollector::collect(
        tileset.loadQueue_,
        tileset.contentLifecycle_.loadLifecycle(),
        tileset.frameResourceBudget_,
        tileset.contentCache_.unloadQueue(),
        tileset.tileRegistry_.tiles());
    if (tileset.terrainProvider_) {
        diagnostics.terrainProviderRequests =
            tileset.terrainProvider_->requestDiagnostics();
    }
    if (tileset.contentProvider_) {
        diagnostics.contentProviderRequests =
            tileset.contentProvider_->requestDiagnostics();
    }
    for (const ActivatedRasterOverlay* overlay : tileset.rasterOverlays_) {
        if (!overlay || !overlay->getTileProvider()) {
            continue;
        }
        addProviderRequestDiagnostics(
            diagnostics.rasterProviderRequests,
            overlay->getTileProvider()->requestDiagnostics());
        diagnostics.rasterOverlayTilesLoading +=
            overlay->getTileProvider()->getThrottledTilesCurrentlyLoading();
        diagnostics.rasterSourceRequestsInFlight +=
            overlay->getTileProvider()->getActiveRasterSourceRequests();
        diagnostics.rasterPendingUploads +=
            overlay->getTileProvider()->getPendingUploadCount();
    }
    return diagnostics;
}

float TilesetQueryFacade::sampleHeight(const Tileset& tileset,
                                       double lngRad,
                                       double latRad) {
    return LoadedTerrainHeightSampler::sampleHeight(
        tileset.tileRegistry_.tiles(),
        tileset.contentLifecycle_.terrainCache(),
        lngRad,
        latRad);
}

} // namespace earth_engine
