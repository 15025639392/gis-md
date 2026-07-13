#pragma once

#include "TileLoadDiagnostics.h"

#include <cstdint>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class TerrainProvider;
class TilesetContentProvider;

struct TilesetProviderDiagnosticsSnapshot {
    ProviderRequestDiagnostics allProviderRequests;
    ProviderRequestDiagnostics terrainProviderRequests;
    ProviderRequestDiagnostics contentProviderRequests;
    ProviderRequestDiagnostics rasterProviderRequests;
    int rasterOverlayTilesLoading = 0;
    int rasterSourceRequestsInFlight = 0;
    int rasterPendingUploads = 0;
    int64_t rasterPendingUploadBytes = 0;
    int64_t rasterCachedSourceTileBytes = 0;

    uint32_t maximumTransportActiveRequests(uint32_t fallback) const;
    void applyTo(TilesetLoadDiagnostics& diagnostics) const;
};

class TilesetProviderDiagnosticsCollector {
public:
    static TilesetProviderDiagnosticsSnapshot collectContentAndRaster(
        const TilesetContentProvider* contentProvider,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);

    static TilesetProviderDiagnosticsSnapshot collect(
        const TerrainProvider* terrainProvider,
        const TilesetContentProvider* contentProvider,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays);
};

} // namespace earth_engine
