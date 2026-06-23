#pragma once

#include "Diagnostics.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../providers/ProviderRequestDiagnostics.h"

namespace earth_engine {

class Tileset;

struct SceneProviderRequestDiagnosticsSnapshot {
    int requestsStarted = 0;
    int requestsCompleted = 0;
    int activeWorkerBlockingRequests = 0;
    int peakWorkerBlockingRequests = 0;
    int transportActiveRequestLimit = -1;
    int externalResourceRequestsStarted = 0;
    int externalResourceRequestsCompleted = 0;
    int activeExternalResourceBlockingRequests = 0;
    int peakExternalResourceBlockingRequests = 0;

    static SceneProviderRequestDiagnosticsSnapshot fromProvider(
        const ProviderRequestDiagnostics& provider);

    void add(const SceneProviderRequestDiagnosticsSnapshot& next);
};

struct SceneFrameResourceBudgetDiagnosticsSnapshot {
    int networkRequestsIssued = 0;
    int networkRequestsLimit = 0;
    int terrainContentNetworkRequestsIssued = 0;
    int terrainContentNetworkRequestsLimit = 0;
    int rasterNetworkRequestsIssued = 0;
    int rasterNetworkRequestsLimit = 0;
    int networkInflightLimit = 0;
    int terrainContentNetworkInflightLimit = 0;
    int rasterNetworkInflightLimit = 0;
    int mainThreadFinalizesUsed = 0;
    int mainThreadFinalizesLimit = 0;
    int terminalStateTransitionsUsed = 0;
    int terminalStateTransitionsLimit = 0;
    int rasterUploadsUsed = 0;
    int rasterUploadsLimit = 0;
    double mainThreadElapsedMs = 0.0;
    double mainThreadTimeLimitMs = 0.0;
    bool interactionActive = false;
    bool smoothingActive = false;

    static SceneFrameResourceBudgetDiagnosticsSnapshot fromBudget(
        const FrameResourceBudgetSnapshot& budget);

    void add(const SceneFrameResourceBudgetDiagnosticsSnapshot& next);
};

struct SceneTilesetDiagnosticsSnapshot {
    int visibleTiles = 0;
    int contentTilesets = 0;
    int contentVisibleTiles = 0;
    int queuedRequests = 0;
    int loadingRequests = 0;
    int loadQueuePreloadRequests = 0;
    int loadQueueNormalRequests = 0;
    int loadQueueUrgentRequests = 0;
    int pendingTerrainRequests = 0;
    int pendingTerrainUploads = 0;
    int pendingTerrainTerminalResults = 0;
    int pendingGltfTerrainUploads = 0;
    int pendingGltfTerrainTerminalResults = 0;
    int pendingContentRequests = 0;
    int pendingContentUploads = 0;
    int pendingContentTerminalResults = 0;
    int rasterOverlayTilesLoading = 0;
    int rasterSourceRequestsInFlight = 0;
    int rasterPendingUploads = 0;
    double lodSizePixels = 0.0;
    int minVisibleZoom = 0;
    int maxVisibleZoom = 0;
    int quadtreeFadingNodes = 0;
    int quadtreeRenderingNodes = 0;
    int quadtreeWalkthroughNodes = 0;
    int quadtreeNotRenderingNodes = 0;
    int quadtreeSelectionRenderedNodes = 0;
    int quadtreeSelectionRefinedNodes = 0;
    int quadtreeSelectionKickedNodes = 0;
    int quadtreeSelectionOccludedNodes = 0;
    int quadtreeSelectionWaitingForOcclusionResultsNodes = 0;
    int quadtreeCulledTilesVisited = 0;
    int quadtreeSelectionAncestorMeetsSseNodes = 0;
    int quadtreeCameraInsideNodes = 0;
    int quadtreeInFrustumNodes = 0;
    int mercatorTileCount = 0;
    int northPolarTileCount = 0;
    int southPolarTileCount = 0;
    int surfaceMeshBytes = 0;
    int terrainCachedTiles = 0;
    int terrainLoadUnloadingTiles = 0;
    int terrainLoadFailedTemporarilyTiles = 0;
    int terrainLoadUnloadedTiles = 0;
    int terrainLoadContentLoadingTiles = 0;
    int terrainLoadContentLoadedTiles = 0;
    int terrainLoadDoneTiles = 0;
    int terrainLoadFailedTiles = 0;
    int terrainContentUnknownTiles = 0;
    int terrainContentEmptyTiles = 0;
    int terrainContentExternalTiles = 0;
    int terrainContentRenderTiles = 0;
    int terrainUnloadQueueTiles = 0;
    int missingRasterOverlayProjections = 0;
    SceneFrameResourceBudgetDiagnosticsSnapshot resourceBudget;
    SceneProviderRequestDiagnosticsSnapshot terrainProviderRequests;
    SceneProviderRequestDiagnosticsSnapshot contentProviderRequests;
    SceneProviderRequestDiagnosticsSnapshot rasterProviderRequests;

    static SceneTilesetDiagnosticsSnapshot fromTileset(
        const Tileset& tileset,
        bool terrain);

    void add(const SceneTilesetDiagnosticsSnapshot& next);
    void applyTo(Diagnostics& diagnostics) const;
};

class SceneTilesetDiagnostics {
public:
    static void reset(Diagnostics& diagnostics);
    static void addTileset(Diagnostics& diagnostics,
                           const Tileset& tileset,
                           bool terrain);
};

} // namespace earth_engine
