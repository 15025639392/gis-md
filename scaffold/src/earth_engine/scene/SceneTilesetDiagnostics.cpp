#include "SceneTilesetDiagnostics.h"

#include "../tiling/Tileset.h"

#include <algorithm>

namespace earth_engine {
namespace {

int toDiagnosticInt(uint32_t value) {
    return static_cast<int>(value);
}

void resetProviderDiagnostics(Diagnostics& diag) {
    diag.terrainProviderRequestsStarted = 0;
    diag.terrainProviderRequestsCompleted = 0;
    diag.terrainProviderActiveWorkerBlockingRequests = 0;
    diag.terrainProviderPeakWorkerBlockingRequests = 0;
    diag.terrainTransportActiveRequestLimit = -1;
    diag.contentProviderRequestsStarted = 0;
    diag.contentProviderRequestsCompleted = 0;
    diag.contentProviderActiveWorkerBlockingRequests = 0;
    diag.contentProviderPeakWorkerBlockingRequests = 0;
    diag.contentProviderExternalResourceRequestsStarted = 0;
    diag.contentProviderExternalResourceRequestsCompleted = 0;
    diag.contentProviderActiveExternalResourceBlockingRequests = 0;
    diag.contentProviderPeakExternalResourceBlockingRequests = 0;
    diag.contentTransportActiveRequestLimit = -1;
    diag.rasterProviderRequestsStarted = 0;
    diag.rasterProviderRequestsCompleted = 0;
    diag.rasterProviderActiveWorkerBlockingRequests = 0;
    diag.rasterProviderPeakWorkerBlockingRequests = 0;
    diag.rasterTransportActiveRequestLimit = -1;
}

void resetResourceBudgetDiagnostics(Diagnostics& diag) {
    diag.budgetNetworkRequestsIssued = 0;
    diag.budgetNetworkRequestsLimit = 0;
    diag.budgetTerrainContentNetworkRequestsIssued = 0;
    diag.budgetTerrainContentNetworkRequestsLimit = 0;
    diag.budgetRasterNetworkRequestsIssued = 0;
    diag.budgetRasterNetworkRequestsLimit = 0;
    diag.budgetMainThreadFinalizesUsed = 0;
    diag.budgetMainThreadFinalizesLimit = 0;
    diag.budgetTerminalStateTransitionsUsed = 0;
    diag.budgetTerminalStateTransitionsLimit = 0;
    diag.budgetRasterUploadsUsed = 0;
    diag.budgetRasterUploadsLimit = 0;
    diag.budgetMainThreadElapsedMs = 0.0;
    diag.budgetMainThreadTimeLimitMs = 0.0;
    diag.budgetInteractionActive = false;
    diag.budgetSmoothingActive = false;
}

void applyResourceBudgetSnapshot(
    Diagnostics& diag,
    const SceneFrameResourceBudgetDiagnosticsSnapshot& budget) {
    diag.budgetNetworkRequestsIssued += budget.networkRequestsIssued;
    diag.budgetNetworkRequestsLimit += budget.networkRequestsLimit;
    diag.budgetTerrainContentNetworkRequestsIssued +=
        budget.terrainContentNetworkRequestsIssued;
    diag.budgetTerrainContentNetworkRequestsLimit +=
        budget.terrainContentNetworkRequestsLimit;
    diag.budgetRasterNetworkRequestsIssued +=
        budget.rasterNetworkRequestsIssued;
    diag.budgetRasterNetworkRequestsLimit +=
        budget.rasterNetworkRequestsLimit;
    diag.budgetMainThreadFinalizesUsed += budget.mainThreadFinalizesUsed;
    diag.budgetMainThreadFinalizesLimit += budget.mainThreadFinalizesLimit;
    diag.budgetTerminalStateTransitionsUsed +=
        budget.terminalStateTransitionsUsed;
    diag.budgetTerminalStateTransitionsLimit +=
        budget.terminalStateTransitionsLimit;
    diag.budgetRasterUploadsUsed += budget.rasterUploadsUsed;
    diag.budgetRasterUploadsLimit += budget.rasterUploadsLimit;
    diag.budgetMainThreadElapsedMs += budget.mainThreadElapsedMs;
    diag.budgetMainThreadTimeLimitMs =
        std::max(diag.budgetMainThreadTimeLimitMs,
                 budget.mainThreadTimeLimitMs);
    diag.budgetInteractionActive =
        diag.budgetInteractionActive || budget.interactionActive;
    diag.budgetSmoothingActive =
        diag.budgetSmoothingActive || budget.smoothingActive;
}

void applyProviderSnapshot(int& requestsStarted,
                           int& requestsCompleted,
                           int& activeWorkerBlockingRequests,
                           int& peakWorkerBlockingRequests,
                           int& transportActiveRequestLimit,
                           const SceneProviderRequestDiagnosticsSnapshot&
                               provider) {
    requestsStarted += provider.requestsStarted;
    requestsCompleted += provider.requestsCompleted;
    activeWorkerBlockingRequests += provider.activeWorkerBlockingRequests;
    peakWorkerBlockingRequests =
        std::max(peakWorkerBlockingRequests,
                 provider.peakWorkerBlockingRequests);
    if (provider.transportActiveRequestLimit >= 0) {
        transportActiveRequestLimit =
            std::max(transportActiveRequestLimit,
                     provider.transportActiveRequestLimit);
    }
}

} // namespace

SceneProviderRequestDiagnosticsSnapshot
SceneProviderRequestDiagnosticsSnapshot::fromProvider(
    const ProviderRequestDiagnostics& provider) {
    SceneProviderRequestDiagnosticsSnapshot snapshot;
    snapshot.requestsStarted = provider.requestsStarted;
    snapshot.requestsCompleted = provider.requestsCompleted;
    snapshot.activeWorkerBlockingRequests =
        provider.activeWorkerBlockingRequests;
    snapshot.peakWorkerBlockingRequests =
        provider.peakWorkerBlockingRequests;
    snapshot.transportActiveRequestLimit =
        provider.maximumTransportActiveRequests;
    snapshot.externalResourceRequestsStarted =
        provider.externalResourceRequestsStarted;
    snapshot.externalResourceRequestsCompleted =
        provider.externalResourceRequestsCompleted;
    snapshot.activeExternalResourceBlockingRequests =
        provider.activeExternalResourceBlockingRequests;
    snapshot.peakExternalResourceBlockingRequests =
        provider.peakExternalResourceBlockingRequests;
    return snapshot;
}

void SceneProviderRequestDiagnosticsSnapshot::add(
    const SceneProviderRequestDiagnosticsSnapshot& next) {
    requestsStarted += next.requestsStarted;
    requestsCompleted += next.requestsCompleted;
    activeWorkerBlockingRequests += next.activeWorkerBlockingRequests;
    peakWorkerBlockingRequests =
        std::max(peakWorkerBlockingRequests,
                 next.peakWorkerBlockingRequests);
    if (next.transportActiveRequestLimit >= 0) {
        transportActiveRequestLimit =
            std::max(transportActiveRequestLimit,
                     next.transportActiveRequestLimit);
    }
    externalResourceRequestsStarted +=
        next.externalResourceRequestsStarted;
    externalResourceRequestsCompleted +=
        next.externalResourceRequestsCompleted;
    activeExternalResourceBlockingRequests +=
        next.activeExternalResourceBlockingRequests;
    peakExternalResourceBlockingRequests =
        std::max(peakExternalResourceBlockingRequests,
                 next.peakExternalResourceBlockingRequests);
}

SceneFrameResourceBudgetDiagnosticsSnapshot
SceneFrameResourceBudgetDiagnosticsSnapshot::fromBudget(
    const FrameResourceBudgetSnapshot& budget) {
    SceneFrameResourceBudgetDiagnosticsSnapshot snapshot;
    snapshot.networkRequestsIssued =
        toDiagnosticInt(budget.networkRequestsIssued);
    snapshot.networkRequestsLimit =
        toDiagnosticInt(budget.maxNetworkRequestsPerFrame);
    snapshot.terrainContentNetworkRequestsIssued =
        toDiagnosticInt(budget.terrainContentNetworkRequestsIssued);
    snapshot.terrainContentNetworkRequestsLimit =
        toDiagnosticInt(budget.maxTerrainContentNetworkRequestsPerFrame);
    snapshot.rasterNetworkRequestsIssued =
        toDiagnosticInt(budget.rasterNetworkRequestsIssued);
    snapshot.rasterNetworkRequestsLimit =
        toDiagnosticInt(budget.maxRasterNetworkRequestsPerFrame);
    snapshot.mainThreadFinalizesUsed =
        toDiagnosticInt(budget.mainThreadFinalizesUsed);
    snapshot.mainThreadFinalizesLimit =
        toDiagnosticInt(budget.maxMainThreadFinalizesPerFrame);
    snapshot.terminalStateTransitionsUsed =
        toDiagnosticInt(budget.terminalStateTransitionsUsed);
    snapshot.terminalStateTransitionsLimit =
        toDiagnosticInt(budget.maxTerminalStateTransitionsPerFrame);
    snapshot.rasterUploadsUsed = toDiagnosticInt(budget.rasterUploadsUsed);
    snapshot.rasterUploadsLimit =
        toDiagnosticInt(budget.maxRasterUploadsPerFrame);
    snapshot.mainThreadElapsedMs = budget.mainThreadElapsedMs;
    snapshot.mainThreadTimeLimitMs = budget.mainThreadTimeMs;
    snapshot.interactionActive = budget.interactionActive;
    snapshot.smoothingActive = budget.smoothingActive;
    return snapshot;
}

void SceneFrameResourceBudgetDiagnosticsSnapshot::add(
    const SceneFrameResourceBudgetDiagnosticsSnapshot& next) {
    networkRequestsIssued += next.networkRequestsIssued;
    networkRequestsLimit += next.networkRequestsLimit;
    terrainContentNetworkRequestsIssued +=
        next.terrainContentNetworkRequestsIssued;
    terrainContentNetworkRequestsLimit +=
        next.terrainContentNetworkRequestsLimit;
    rasterNetworkRequestsIssued += next.rasterNetworkRequestsIssued;
    rasterNetworkRequestsLimit += next.rasterNetworkRequestsLimit;
    mainThreadFinalizesUsed += next.mainThreadFinalizesUsed;
    mainThreadFinalizesLimit += next.mainThreadFinalizesLimit;
    terminalStateTransitionsUsed += next.terminalStateTransitionsUsed;
    terminalStateTransitionsLimit += next.terminalStateTransitionsLimit;
    rasterUploadsUsed += next.rasterUploadsUsed;
    rasterUploadsLimit += next.rasterUploadsLimit;
    mainThreadElapsedMs += next.mainThreadElapsedMs;
    mainThreadTimeLimitMs =
        std::max(mainThreadTimeLimitMs, next.mainThreadTimeLimitMs);
    interactionActive = interactionActive || next.interactionActive;
    smoothingActive = smoothingActive || next.smoothingActive;
}

SceneTilesetDiagnosticsSnapshot
SceneTilesetDiagnosticsSnapshot::fromTileset(
    const Tileset& tileset,
    bool terrain) {
    SceneTilesetDiagnosticsSnapshot snapshot;
    const TilePlan& plan = tileset.tilePlan();
    const TilesetLoadDiagnostics loadDiag = tileset.loadDiagnostics();

    if (terrain) {
        snapshot.visibleTiles = static_cast<int>(plan.visibleTiles.size());
        snapshot.terrainCachedTiles = tileset.cachedTerrainTiles();
        snapshot.pendingTerrainRequests = loadDiag.pendingTerrainRequests;
        snapshot.pendingTerrainUploads = loadDiag.pendingTerrainUploads;
        snapshot.pendingTerrainTerminalResults =
            loadDiag.pendingTerrainTerminalResults;
        snapshot.minVisibleZoom = plan.minVisibleZoom;
        snapshot.maxVisibleZoom = plan.maxVisibleZoom;
        snapshot.lodSizePixels = plan.lodSizePixels;
        snapshot.quadtreeRenderingNodes = plan.renderingNodeCount;
        snapshot.quadtreeWalkthroughNodes = plan.walkthroughNodeCount;
        snapshot.quadtreeNotRenderingNodes = plan.notRenderingNodeCount;
        snapshot.quadtreeSelectionRenderedNodes =
            plan.selectionRenderedCount;
        snapshot.quadtreeSelectionRefinedNodes =
            plan.selectionRefinedCount;
        snapshot.quadtreeSelectionKickedNodes = plan.selectionKickedCount;
        snapshot.quadtreeSelectionOccludedNodes =
            plan.selectionOccludedCount;
        snapshot.quadtreeSelectionWaitingForOcclusionResultsNodes =
            plan.selectionWaitingForOcclusionResultsCount;
        snapshot.quadtreeCulledTilesVisited =
            plan.culledTilesVisitedCount;
        snapshot.quadtreeSelectionAncestorMeetsSseNodes =
            plan.selectionAncestorMeetsSseCount;
        snapshot.quadtreeFadingNodes = plan.fadingNodeCount;
        snapshot.quadtreeCameraInsideNodes = plan.cameraInsideNodeCount;
        snapshot.quadtreeInFrustumNodes = plan.inFrustumNodeCount;
        snapshot.mercatorTileCount = plan.mercatorTileCount;
        snapshot.northPolarTileCount = plan.northPolarTileCount;
        snapshot.southPolarTileCount = plan.southPolarTileCount;
        snapshot.terrainLoadUnloadingTiles = loadDiag.loadUnloadingTiles;
        snapshot.terrainLoadFailedTemporarilyTiles =
            loadDiag.loadFailedTemporarilyTiles;
        snapshot.terrainLoadUnloadedTiles = loadDiag.loadUnloadedTiles;
        snapshot.terrainLoadContentLoadingTiles =
            loadDiag.loadContentLoadingTiles;
        snapshot.terrainLoadContentLoadedTiles =
            loadDiag.loadContentLoadedTiles;
        snapshot.terrainLoadDoneTiles = loadDiag.loadDoneTiles;
        snapshot.terrainLoadFailedTiles = loadDiag.loadFailedTiles;
        snapshot.terrainUnloadQueueTiles = loadDiag.unloadQueueTiles;
        snapshot.missingRasterOverlayProjections =
            loadDiag.missingRasterOverlayProjections;
    } else {
        snapshot.contentVisibleTiles =
            static_cast<int>(plan.visibleTiles.size());
    }

    snapshot.queuedRequests = loadDiag.loadQueueTotal();
    snapshot.loadingRequests =
        loadDiag.pendingTerrainTotal() + loadDiag.pendingContentTotal();
    snapshot.loadQueuePreloadRequests = loadDiag.loadQueuePreloadRequests;
    snapshot.loadQueueNormalRequests = loadDiag.loadQueueNormalRequests;
    snapshot.loadQueueUrgentRequests = loadDiag.loadQueueUrgentRequests;
    snapshot.pendingContentRequests = loadDiag.pendingContentRequests;
    snapshot.pendingContentUploads = loadDiag.pendingContentUploads;
    snapshot.pendingContentTerminalResults =
        loadDiag.pendingContentTerminalResults;
    snapshot.rasterOverlayTilesLoading =
        loadDiag.rasterOverlayTilesLoading;
    snapshot.rasterSourceRequestsInFlight =
        loadDiag.rasterSourceRequestsInFlight;
    snapshot.rasterPendingUploads = loadDiag.rasterPendingUploads;
    snapshot.surfaceMeshBytes =
        static_cast<int>(tileset.totalBytesUsed());
    snapshot.terrainContentUnknownTiles = loadDiag.contentUnknownTiles;
    snapshot.terrainContentEmptyTiles = loadDiag.contentEmptyTiles;
    snapshot.terrainContentExternalTiles = loadDiag.contentExternalTiles;
    snapshot.terrainContentRenderTiles = loadDiag.contentRenderTiles;
    snapshot.resourceBudget =
        SceneFrameResourceBudgetDiagnosticsSnapshot::fromBudget(
            loadDiag.resourceBudget);
    snapshot.terrainProviderRequests =
        SceneProviderRequestDiagnosticsSnapshot::fromProvider(
            loadDiag.terrainProviderRequests);
    snapshot.contentProviderRequests =
        SceneProviderRequestDiagnosticsSnapshot::fromProvider(
            loadDiag.contentProviderRequests);
    snapshot.rasterProviderRequests =
        SceneProviderRequestDiagnosticsSnapshot::fromProvider(
            loadDiag.rasterProviderRequests);
    return snapshot;
}

void SceneTilesetDiagnosticsSnapshot::add(
    const SceneTilesetDiagnosticsSnapshot& next) {
    visibleTiles = next.visibleTiles != 0 ? next.visibleTiles : visibleTiles;
    contentTilesets += next.contentTilesets;
    contentVisibleTiles += next.contentVisibleTiles;
    queuedRequests += next.queuedRequests;
    loadingRequests += next.loadingRequests;
    loadQueuePreloadRequests += next.loadQueuePreloadRequests;
    loadQueueNormalRequests += next.loadQueueNormalRequests;
    loadQueueUrgentRequests += next.loadQueueUrgentRequests;
    pendingTerrainRequests = next.pendingTerrainRequests != 0
                                 ? next.pendingTerrainRequests
                                 : pendingTerrainRequests;
    pendingTerrainUploads = next.pendingTerrainUploads != 0
                                ? next.pendingTerrainUploads
                                : pendingTerrainUploads;
    pendingTerrainTerminalResults =
        next.pendingTerrainTerminalResults != 0
            ? next.pendingTerrainTerminalResults
            : pendingTerrainTerminalResults;
    pendingContentRequests += next.pendingContentRequests;
    pendingContentUploads += next.pendingContentUploads;
    pendingContentTerminalResults += next.pendingContentTerminalResults;
    rasterOverlayTilesLoading += next.rasterOverlayTilesLoading;
    rasterSourceRequestsInFlight += next.rasterSourceRequestsInFlight;
    rasterPendingUploads += next.rasterPendingUploads;
    lodSizePixels = next.lodSizePixels != 0.0 ? next.lodSizePixels
                                              : lodSizePixels;
    minVisibleZoom =
        next.minVisibleZoom != 0 ? next.minVisibleZoom : minVisibleZoom;
    maxVisibleZoom =
        next.maxVisibleZoom != 0 ? next.maxVisibleZoom : maxVisibleZoom;
    quadtreeFadingNodes =
        next.quadtreeFadingNodes != 0 ? next.quadtreeFadingNodes
                                      : quadtreeFadingNodes;
    quadtreeRenderingNodes =
        next.quadtreeRenderingNodes != 0 ? next.quadtreeRenderingNodes
                                         : quadtreeRenderingNodes;
    quadtreeWalkthroughNodes =
        next.quadtreeWalkthroughNodes != 0 ? next.quadtreeWalkthroughNodes
                                           : quadtreeWalkthroughNodes;
    quadtreeNotRenderingNodes =
        next.quadtreeNotRenderingNodes != 0 ? next.quadtreeNotRenderingNodes
                                            : quadtreeNotRenderingNodes;
    quadtreeSelectionRenderedNodes =
        next.quadtreeSelectionRenderedNodes != 0
            ? next.quadtreeSelectionRenderedNodes
            : quadtreeSelectionRenderedNodes;
    quadtreeSelectionRefinedNodes =
        next.quadtreeSelectionRefinedNodes != 0
            ? next.quadtreeSelectionRefinedNodes
            : quadtreeSelectionRefinedNodes;
    quadtreeSelectionKickedNodes =
        next.quadtreeSelectionKickedNodes != 0
            ? next.quadtreeSelectionKickedNodes
            : quadtreeSelectionKickedNodes;
    quadtreeSelectionOccludedNodes =
        next.quadtreeSelectionOccludedNodes != 0
            ? next.quadtreeSelectionOccludedNodes
            : quadtreeSelectionOccludedNodes;
    quadtreeSelectionWaitingForOcclusionResultsNodes =
        next.quadtreeSelectionWaitingForOcclusionResultsNodes != 0
            ? next.quadtreeSelectionWaitingForOcclusionResultsNodes
            : quadtreeSelectionWaitingForOcclusionResultsNodes;
    quadtreeCulledTilesVisited =
        next.quadtreeCulledTilesVisited != 0
            ? next.quadtreeCulledTilesVisited
            : quadtreeCulledTilesVisited;
    quadtreeSelectionAncestorMeetsSseNodes =
        next.quadtreeSelectionAncestorMeetsSseNodes != 0
            ? next.quadtreeSelectionAncestorMeetsSseNodes
            : quadtreeSelectionAncestorMeetsSseNodes;
    quadtreeCameraInsideNodes =
        next.quadtreeCameraInsideNodes != 0
            ? next.quadtreeCameraInsideNodes
            : quadtreeCameraInsideNodes;
    quadtreeInFrustumNodes =
        next.quadtreeInFrustumNodes != 0 ? next.quadtreeInFrustumNodes
                                         : quadtreeInFrustumNodes;
    mercatorTileCount =
        next.mercatorTileCount != 0 ? next.mercatorTileCount
                                    : mercatorTileCount;
    northPolarTileCount =
        next.northPolarTileCount != 0 ? next.northPolarTileCount
                                      : northPolarTileCount;
    southPolarTileCount =
        next.southPolarTileCount != 0 ? next.southPolarTileCount
                                      : southPolarTileCount;
    surfaceMeshBytes += next.surfaceMeshBytes;
    terrainCachedTiles =
        next.terrainCachedTiles != 0 ? next.terrainCachedTiles
                                     : terrainCachedTiles;
    terrainLoadUnloadingTiles =
        next.terrainLoadUnloadingTiles != 0
            ? next.terrainLoadUnloadingTiles
            : terrainLoadUnloadingTiles;
    terrainLoadFailedTemporarilyTiles =
        next.terrainLoadFailedTemporarilyTiles != 0
            ? next.terrainLoadFailedTemporarilyTiles
            : terrainLoadFailedTemporarilyTiles;
    terrainLoadUnloadedTiles =
        next.terrainLoadUnloadedTiles != 0 ? next.terrainLoadUnloadedTiles
                                           : terrainLoadUnloadedTiles;
    terrainLoadContentLoadingTiles =
        next.terrainLoadContentLoadingTiles != 0
            ? next.terrainLoadContentLoadingTiles
            : terrainLoadContentLoadingTiles;
    terrainLoadContentLoadedTiles =
        next.terrainLoadContentLoadedTiles != 0
            ? next.terrainLoadContentLoadedTiles
            : terrainLoadContentLoadedTiles;
    terrainLoadDoneTiles =
        next.terrainLoadDoneTiles != 0 ? next.terrainLoadDoneTiles
                                       : terrainLoadDoneTiles;
    terrainLoadFailedTiles =
        next.terrainLoadFailedTiles != 0 ? next.terrainLoadFailedTiles
                                         : terrainLoadFailedTiles;
    terrainContentUnknownTiles += next.terrainContentUnknownTiles;
    terrainContentEmptyTiles += next.terrainContentEmptyTiles;
    terrainContentExternalTiles += next.terrainContentExternalTiles;
    terrainContentRenderTiles += next.terrainContentRenderTiles;
    terrainUnloadQueueTiles =
        next.terrainUnloadQueueTiles != 0 ? next.terrainUnloadQueueTiles
                                          : terrainUnloadQueueTiles;
    missingRasterOverlayProjections =
        next.missingRasterOverlayProjections != 0
            ? next.missingRasterOverlayProjections
            : missingRasterOverlayProjections;
    resourceBudget.add(next.resourceBudget);
    terrainProviderRequests.add(next.terrainProviderRequests);
    contentProviderRequests.add(next.contentProviderRequests);
    rasterProviderRequests.add(next.rasterProviderRequests);
}

void SceneTilesetDiagnosticsSnapshot::applyTo(Diagnostics& diag) const {
    diag.visibleTiles = visibleTiles;
    diag.contentTilesets += contentTilesets;
    diag.contentVisibleTiles += contentVisibleTiles;
    diag.queuedRequests += queuedRequests;
    diag.loadingRequests += loadingRequests;
    diag.loadQueuePreloadRequests += loadQueuePreloadRequests;
    diag.loadQueueNormalRequests += loadQueueNormalRequests;
    diag.loadQueueUrgentRequests += loadQueueUrgentRequests;
    diag.pendingTerrainRequests = pendingTerrainRequests;
    diag.pendingTerrainUploads = pendingTerrainUploads;
    diag.pendingTerrainTerminalResults = pendingTerrainTerminalResults;
    diag.pendingContentRequests += pendingContentRequests;
    diag.pendingContentUploads += pendingContentUploads;
    diag.pendingContentTerminalResults += pendingContentTerminalResults;
    diag.rasterOverlayTilesLoading += rasterOverlayTilesLoading;
    diag.rasterSourceRequestsInFlight += rasterSourceRequestsInFlight;
    diag.rasterPendingUploads += rasterPendingUploads;
    diag.lodSizePixels = lodSizePixels;
    diag.minVisibleZoom = minVisibleZoom;
    diag.maxVisibleZoom = maxVisibleZoom;
    diag.quadtreeFadingNodes = quadtreeFadingNodes;
    diag.quadtreeRenderingNodes = quadtreeRenderingNodes;
    diag.quadtreeWalkthroughNodes = quadtreeWalkthroughNodes;
    diag.quadtreeNotRenderingNodes = quadtreeNotRenderingNodes;
    diag.quadtreeSelectionRenderedNodes = quadtreeSelectionRenderedNodes;
    diag.quadtreeSelectionRefinedNodes = quadtreeSelectionRefinedNodes;
    diag.quadtreeSelectionKickedNodes = quadtreeSelectionKickedNodes;
    diag.quadtreeSelectionOccludedNodes = quadtreeSelectionOccludedNodes;
    diag.quadtreeSelectionWaitingForOcclusionResultsNodes =
        quadtreeSelectionWaitingForOcclusionResultsNodes;
    diag.quadtreeCulledTilesVisited = quadtreeCulledTilesVisited;
    diag.quadtreeSelectionAncestorMeetsSseNodes =
        quadtreeSelectionAncestorMeetsSseNodes;
    diag.quadtreeCameraInsideNodes = quadtreeCameraInsideNodes;
    diag.quadtreeInFrustumNodes = quadtreeInFrustumNodes;
    diag.mercatorTileCount = mercatorTileCount;
    diag.northPolarTileCount = northPolarTileCount;
    diag.southPolarTileCount = southPolarTileCount;
    diag.surfaceMeshBytes += surfaceMeshBytes;
    diag.terrainCachedTiles = terrainCachedTiles;
    diag.terrainLoadUnloadingTiles = terrainLoadUnloadingTiles;
    diag.terrainLoadFailedTemporarilyTiles =
        terrainLoadFailedTemporarilyTiles;
    diag.terrainLoadUnloadedTiles = terrainLoadUnloadedTiles;
    diag.terrainLoadContentLoadingTiles = terrainLoadContentLoadingTiles;
    diag.terrainLoadContentLoadedTiles = terrainLoadContentLoadedTiles;
    diag.terrainLoadDoneTiles = terrainLoadDoneTiles;
    diag.terrainLoadFailedTiles = terrainLoadFailedTiles;
    diag.terrainContentUnknownTiles += terrainContentUnknownTiles;
    diag.terrainContentEmptyTiles += terrainContentEmptyTiles;
    diag.terrainContentExternalTiles += terrainContentExternalTiles;
    diag.terrainContentRenderTiles += terrainContentRenderTiles;
    diag.terrainUnloadQueueTiles = terrainUnloadQueueTiles;
    diag.missingRasterOverlayProjections = missingRasterOverlayProjections;
    applyResourceBudgetSnapshot(diag, resourceBudget);
    applyProviderSnapshot(
        diag.terrainProviderRequestsStarted,
        diag.terrainProviderRequestsCompleted,
        diag.terrainProviderActiveWorkerBlockingRequests,
        diag.terrainProviderPeakWorkerBlockingRequests,
        diag.terrainTransportActiveRequestLimit,
        terrainProviderRequests);
    applyProviderSnapshot(
        diag.contentProviderRequestsStarted,
        diag.contentProviderRequestsCompleted,
        diag.contentProviderActiveWorkerBlockingRequests,
        diag.contentProviderPeakWorkerBlockingRequests,
        diag.contentTransportActiveRequestLimit,
        contentProviderRequests);
    diag.contentProviderExternalResourceRequestsStarted +=
        contentProviderRequests.externalResourceRequestsStarted;
    diag.contentProviderExternalResourceRequestsCompleted +=
        contentProviderRequests.externalResourceRequestsCompleted;
    diag.contentProviderActiveExternalResourceBlockingRequests +=
        contentProviderRequests.activeExternalResourceBlockingRequests;
    diag.contentProviderPeakExternalResourceBlockingRequests =
        std::max(
            diag.contentProviderPeakExternalResourceBlockingRequests,
            contentProviderRequests.peakExternalResourceBlockingRequests);
    applyProviderSnapshot(
        diag.rasterProviderRequestsStarted,
        diag.rasterProviderRequestsCompleted,
        diag.rasterProviderActiveWorkerBlockingRequests,
        diag.rasterProviderPeakWorkerBlockingRequests,
        diag.rasterTransportActiveRequestLimit,
        rasterProviderRequests);
}

void SceneTilesetDiagnostics::reset(Diagnostics& diag) {
    diag.visibleTiles = 0;
    diag.contentTilesets = 0;
    diag.contentVisibleTiles = 0;
    diag.cachedTextures = 0;
    diag.queuedRequests = 0;
    diag.loadingRequests = 0;
    diag.loadQueuePreloadRequests = 0;
    diag.loadQueueNormalRequests = 0;
    diag.loadQueueUrgentRequests = 0;
    resetResourceBudgetDiagnostics(diag);
    diag.pendingTerrainRequests = 0;
    diag.pendingTerrainUploads = 0;
    diag.pendingTerrainTerminalResults = 0;
    diag.pendingContentRequests = 0;
    diag.pendingContentUploads = 0;
    diag.pendingContentTerminalResults = 0;
    resetProviderDiagnostics(diag);
    diag.rasterOverlayTilesLoading = 0;
    diag.rasterSourceRequestsInFlight = 0;
    diag.rasterPendingUploads = 0;
    diag.lodSizePixels = 0.0;
    diag.minVisibleZoom = 0;
    diag.maxVisibleZoom = 0;
    diag.quadtreeEqualZoomLayers = 0;
    diag.quadtreeFadingNodes = 0;
    diag.quadtreeNeighborLinks = 0;
    diag.quadtreeNeighborBalancedTiles = 0;
    diag.quadtreeRenderingNodes = 0;
    diag.quadtreeWalkthroughNodes = 0;
    diag.quadtreeNotRenderingNodes = 0;
    diag.quadtreeSelectionRenderedNodes = 0;
    diag.quadtreeSelectionRefinedNodes = 0;
    diag.quadtreeSelectionKickedNodes = 0;
    diag.quadtreeSelectionOccludedNodes = 0;
    diag.quadtreeSelectionWaitingForOcclusionResultsNodes = 0;
    diag.quadtreeCulledTilesVisited = 0;
    diag.quadtreeSelectionAncestorMeetsSseNodes = 0;
    diag.quadtreeCameraInsideNodes = 0;
    diag.quadtreeInFrustumNodes = 0;
    diag.quadtreeHorizonTangentPreservedNodes = 0;
    diag.quadtreeEqualZoomSecondPassNodes = 0;
    diag.mercatorTileCount = 0;
    diag.northPolarTileCount = 0;
    diag.southPolarTileCount = 0;
    diag.surfaceMeshBytes = 0;
    diag.terrainCachedTiles = 0;
    diag.terrainLoadUnloadingTiles = 0;
    diag.terrainLoadFailedTemporarilyTiles = 0;
    diag.terrainLoadUnloadedTiles = 0;
    diag.terrainLoadContentLoadingTiles = 0;
    diag.terrainLoadContentLoadedTiles = 0;
    diag.terrainLoadDoneTiles = 0;
    diag.terrainLoadFailedTiles = 0;
    diag.terrainContentUnknownTiles = 0;
    diag.terrainContentEmptyTiles = 0;
    diag.terrainContentExternalTiles = 0;
    diag.terrainContentRenderTiles = 0;
    diag.terrainUnloadQueueTiles = 0;
    diag.missingRasterOverlayProjections = 0;
}

void SceneTilesetDiagnostics::addTileset(Diagnostics& diag,
                                         const Tileset& tileset,
                                         bool terrain) {
    SceneTilesetDiagnosticsSnapshot::fromTileset(tileset, terrain)
        .applyTo(diag);
}

} // namespace earth_engine
