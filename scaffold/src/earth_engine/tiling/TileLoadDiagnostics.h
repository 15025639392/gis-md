#pragma once

#include "TileLoadTypes.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../providers/ProviderRequestDiagnostics.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

class TileLoadLifecycle;
class TileLoadQueue;
class TileUnloadQueue;
struct TilesetTile;

struct TilesetLoadDiagnostics {
    int loadQueuePreloadRequests = 0;
    int loadQueueNormalRequests = 0;
    int loadQueueUrgentRequests = 0;
    int pendingTerrainRequests = 0;
    int pendingGltfTerrainUploads = 0;
    int pendingGltfTerrainTerminalResults = 0;
    int pendingContentRequests = 0;
    int pendingContentUploads = 0;
    int pendingContentTerminalResults = 0;
    int rasterOverlayTilesLoading = 0;
    int rasterSourceRequestsInFlight = 0;
    int rasterPendingUploads = 0;
    int64_t rasterPendingUploadBytes = 0;
    int64_t peakRasterPendingUploadBytes = 0;
    int64_t rasterCachedSourceTileBytes = 0;
    int64_t peakRasterCachedSourceTileBytes = 0;
    int unloadQueueTiles = 0;
    int loadUnloadingTiles = 0;
    int loadFailedTemporarilyTiles = 0;
    int loadUnloadedTiles = 0;
    int loadContentLoadingTiles = 0;
    int loadContentLoadedTiles = 0;
    int loadDoneTiles = 0;
    int loadFailedTiles = 0;
    int contentUnknownTiles = 0;
    int contentEmptyTiles = 0;
    int contentExternalTiles = 0;
    int contentRenderTiles = 0;
    int missingRasterOverlayProjections = 0;
    ProviderRequestDiagnostics terrainProviderRequests;
    ProviderRequestDiagnostics contentProviderRequests;
    ProviderRequestDiagnostics rasterProviderRequests;
    FrameResourceBudgetSnapshot resourceBudget;
    TileLoadRequestOutcome lastRequestOutcome;

    int loadQueueTotal() const;
    int pendingTerrainTotal() const;
    int pendingContentTotal() const;
};

struct TileLoadDiagnosticsCollector {
    static TilesetLoadDiagnostics collect(
        const TileLoadQueue& loadQueue,
        const TileLoadLifecycle& loadLifecycle,
        const FrameResourceBudget& resourceBudget,
        const TileUnloadQueue& unloadQueue,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles);
};

} // namespace earth_engine
