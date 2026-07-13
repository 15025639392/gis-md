#include "SceneTilesetDiagnostics.h"

#include "../tiling/Tileset.h"

#include <algorithm>
#include <limits>

namespace earth_engine {
namespace {

int toDiagnosticInt(uint32_t value) {
    constexpr uint32_t maxInt =
        static_cast<uint32_t>(std::numeric_limits<int>::max());
    return value > maxInt
               ? std::numeric_limits<int>::max()
               : static_cast<int>(value);
}

void addSaturating(int& target, int value) {
    if (value <= 0) {
        return;
    }
    const int available = std::numeric_limits<int>::max() - target;
    target += std::min(value, available);
}

int saturatingAddInt(int left, int right) {
    addSaturating(left, right);
    return left;
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
    diag.budgetContentNetworkRequestsIssued = 0;
    diag.budgetContentNetworkRequestsLimit = 0;
    diag.budgetRasterNetworkRequestsIssued = 0;
    diag.budgetRasterNetworkRequestsLimit = 0;
    diag.budgetNetworkInflightLimit = 0;
    diag.budgetTerrainContentNetworkInflightLimit = 0;
    diag.budgetContentNetworkInflightLimit = 0;
    diag.budgetRasterNetworkInflightLimit = 0;
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
    addSaturating(diag.budgetNetworkRequestsIssued,
                  budget.networkRequestsIssued);
    addSaturating(diag.budgetNetworkRequestsLimit,
                  budget.networkRequestsLimit);
    addSaturating(diag.budgetTerrainContentNetworkRequestsIssued,
                  budget.terrainContentNetworkRequestsIssued);
    addSaturating(diag.budgetTerrainContentNetworkRequestsLimit,
                  budget.terrainContentNetworkRequestsLimit);
    addSaturating(diag.budgetContentNetworkRequestsIssued,
                  budget.contentNetworkRequestsIssued);
    addSaturating(diag.budgetContentNetworkRequestsLimit,
                  budget.contentNetworkRequestsLimit);
    addSaturating(diag.budgetRasterNetworkRequestsIssued,
                  budget.rasterNetworkRequestsIssued);
    addSaturating(diag.budgetRasterNetworkRequestsLimit,
                  budget.rasterNetworkRequestsLimit);
    addSaturating(diag.budgetNetworkInflightLimit,
                  budget.networkInflightLimit);
    addSaturating(diag.budgetTerrainContentNetworkInflightLimit,
                  budget.terrainContentNetworkInflightLimit);
    addSaturating(diag.budgetContentNetworkInflightLimit,
                  budget.contentNetworkInflightLimit);
    addSaturating(diag.budgetRasterNetworkInflightLimit,
                  budget.rasterNetworkInflightLimit);
    addSaturating(diag.budgetMainThreadFinalizesUsed,
                  budget.mainThreadFinalizesUsed);
    addSaturating(diag.budgetMainThreadFinalizesLimit,
                  budget.mainThreadFinalizesLimit);
    addSaturating(diag.budgetTerminalStateTransitionsUsed,
                  budget.terminalStateTransitionsUsed);
    addSaturating(diag.budgetTerminalStateTransitionsLimit,
                  budget.terminalStateTransitionsLimit);
    addSaturating(diag.budgetRasterUploadsUsed,
                  budget.rasterUploadsUsed);
    addSaturating(diag.budgetRasterUploadsLimit,
                  budget.rasterUploadsLimit);
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
    snapshot.requestsFailed = provider.requestsFailed;
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
    snapshot.externalResourceRequestsFailed =
        provider.externalResourceRequestsFailed;
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
    requestsFailed += next.requestsFailed;
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
    externalResourceRequestsFailed +=
        next.externalResourceRequestsFailed;
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
    snapshot.contentNetworkRequestsIssued =
        toDiagnosticInt(budget.contentNetworkRequestsIssued);
    snapshot.contentNetworkRequestsLimit =
        toDiagnosticInt(budget.maxContentNetworkRequestsPerFrame);
    snapshot.rasterNetworkRequestsIssued =
        toDiagnosticInt(budget.rasterNetworkRequestsIssued);
    snapshot.rasterNetworkRequestsLimit =
        toDiagnosticInt(budget.maxRasterNetworkRequestsPerFrame);
    snapshot.networkInflightLimit = toDiagnosticInt(budget.maxNetworkInflight);
    snapshot.terrainContentNetworkInflightLimit =
        toDiagnosticInt(budget.maxTerrainContentNetworkInflight);
    snapshot.contentNetworkInflightLimit =
        toDiagnosticInt(budget.maxContentNetworkInflight);
    snapshot.rasterNetworkInflightLimit =
        toDiagnosticInt(budget.maxRasterNetworkInflight);
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
    addSaturating(networkRequestsIssued, next.networkRequestsIssued);
    addSaturating(networkRequestsLimit, next.networkRequestsLimit);
    addSaturating(terrainContentNetworkRequestsIssued,
                  next.terrainContentNetworkRequestsIssued);
    addSaturating(terrainContentNetworkRequestsLimit,
                  next.terrainContentNetworkRequestsLimit);
    addSaturating(contentNetworkRequestsIssued,
                  next.contentNetworkRequestsIssued);
    addSaturating(contentNetworkRequestsLimit,
                  next.contentNetworkRequestsLimit);
    addSaturating(rasterNetworkRequestsIssued,
                  next.rasterNetworkRequestsIssued);
    addSaturating(rasterNetworkRequestsLimit, next.rasterNetworkRequestsLimit);
    addSaturating(networkInflightLimit, next.networkInflightLimit);
    addSaturating(terrainContentNetworkInflightLimit,
                  next.terrainContentNetworkInflightLimit);
    addSaturating(contentNetworkInflightLimit,
                  next.contentNetworkInflightLimit);
    addSaturating(rasterNetworkInflightLimit,
                  next.rasterNetworkInflightLimit);
    addSaturating(mainThreadFinalizesUsed, next.mainThreadFinalizesUsed);
    addSaturating(mainThreadFinalizesLimit, next.mainThreadFinalizesLimit);
    addSaturating(terminalStateTransitionsUsed,
                  next.terminalStateTransitionsUsed);
    addSaturating(terminalStateTransitionsLimit,
                  next.terminalStateTransitionsLimit);
    addSaturating(rasterUploadsUsed, next.rasterUploadsUsed);
    addSaturating(rasterUploadsLimit, next.rasterUploadsLimit);
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
        snapshot.terrainCachedTiles =
            tileset.cachedHeightmapTerrainTilesForLegacySurfacePath();
        snapshot.pendingTerrainRequests = loadDiag.pendingTerrainRequests;
        snapshot.pendingGltfTerrainUploads =
            loadDiag.pendingGltfTerrainUploads;
        snapshot.pendingGltfTerrainTerminalResults =
            loadDiag.pendingGltfTerrainTerminalResults;
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
    const TileLoadRequestOutcome& outcome = loadDiag.lastRequestOutcome;
    snapshot.requestIssued = static_cast<int>(outcome.issued);
    snapshot.requestBlockedByInflight = outcome.blockedByInflight ? 1 : 0;
    snapshot.reqSkipEmptyKey = static_cast<int>(outcome.skippedEmptyCacheKey);
    snapshot.reqSkipAlreadyPending =
        static_cast<int>(outcome.skippedAlreadyPending);
    snapshot.reqSkipEmptyTile = static_cast<int>(outcome.skippedEmptyTile);
    snapshot.reqSkipClassified = static_cast<int>(outcome.skippedClassified);
    snapshot.reqSkipUpsampleSrc =
        static_cast<int>(outcome.skippedUpsampleSourceNotReady);
    snapshot.reqSkipUpsampleNoContent =
        static_cast<int>(outcome.skippedUpsampleNoContentSource);
    snapshot.reqSkipDispatch = static_cast<int>(outcome.skippedDispatch);
    snapshot.reqSkipNoProvider =
        static_cast<int>(outcome.skippedNoContentProvider);
    snapshot.reqStopDispatch = static_cast<int>(outcome.stoppedAtDispatch);
    snapshot.rasterOverlayTilesLoading =
        loadDiag.rasterOverlayTilesLoading;
    snapshot.rasterSourceRequestsInFlight =
        loadDiag.rasterSourceRequestsInFlight;
    snapshot.rasterPendingUploads = loadDiag.rasterPendingUploads;
    snapshot.rasterPendingUploadBytes =
        static_cast<int>(std::min<int64_t>(
            loadDiag.rasterPendingUploadBytes,
            std::numeric_limits<int>::max()));
    snapshot.rasterCachedSourceTileBytes =
        static_cast<int>(std::min<int64_t>(
            loadDiag.rasterCachedSourceTileBytes,
            std::numeric_limits<int>::max()));
    snapshot.frameMappedRasterTileCount =
        plan.frameMappedRasterTileCount;
    snapshot.frameMappedRasterTileLoadingCount =
        plan.frameMappedRasterTileLoadingCount;
    snapshot.frameProgressTotalCount = plan.frameProgressTotalCount;
    snapshot.frameProgressLoadingCount =
        plan.frameProgressLoadingCount;
    snapshot.frameLoadProgressPercentage =
        plan.frameLoadProgressPercentage;
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
    pendingGltfTerrainUploads += next.pendingGltfTerrainUploads;
    pendingGltfTerrainTerminalResults +=
        next.pendingGltfTerrainTerminalResults;
    pendingContentRequests += next.pendingContentRequests;
    pendingContentUploads += next.pendingContentUploads;
    pendingContentTerminalResults += next.pendingContentTerminalResults;
    requestIssued += next.requestIssued;
    requestBlockedByInflight += next.requestBlockedByInflight;
    reqSkipEmptyKey += next.reqSkipEmptyKey;
    reqSkipAlreadyPending += next.reqSkipAlreadyPending;
    reqSkipEmptyTile += next.reqSkipEmptyTile;
    reqSkipClassified += next.reqSkipClassified;
    reqSkipUpsampleSrc += next.reqSkipUpsampleSrc;
    reqSkipUpsampleNoContent += next.reqSkipUpsampleNoContent;
    reqSkipDispatch += next.reqSkipDispatch;
    reqSkipNoProvider += next.reqSkipNoProvider;
    reqStopDispatch += next.reqStopDispatch;
    rasterOverlayTilesLoading += next.rasterOverlayTilesLoading;
    rasterSourceRequestsInFlight += next.rasterSourceRequestsInFlight;
    rasterPendingUploads += next.rasterPendingUploads;
    rasterPendingUploadBytes = saturatingAddInt(
        rasterPendingUploadBytes,
        next.rasterPendingUploadBytes);
    rasterCachedSourceTileBytes = saturatingAddInt(
        rasterCachedSourceTileBytes,
        next.rasterCachedSourceTileBytes);
    frameMappedRasterTileCount += next.frameMappedRasterTileCount;
    frameMappedRasterTileLoadingCount +=
        next.frameMappedRasterTileLoadingCount;
    frameProgressTotalCount += next.frameProgressTotalCount;
    frameProgressLoadingCount += next.frameProgressLoadingCount;
    frameLoadProgressPercentage =
        frameProgressLoadingCount == 0 || frameProgressTotalCount <= 0
            ? 100.0
            : 100.0 *
                  static_cast<double>(frameProgressTotalCount -
                                      frameProgressLoadingCount) /
                  static_cast<double>(frameProgressTotalCount);
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
    diag.pendingGltfTerrainUploads += pendingGltfTerrainUploads;
    diag.pendingGltfTerrainTerminalResults +=
        pendingGltfTerrainTerminalResults;
    diag.pendingContentRequests += pendingContentRequests;
    diag.pendingContentUploads += pendingContentUploads;
    diag.pendingContentTerminalResults += pendingContentTerminalResults;
    diag.requestIssued += requestIssued;
    diag.requestBlockedByInflight += requestBlockedByInflight;
    diag.reqSkipEmptyKey += reqSkipEmptyKey;
    diag.reqSkipAlreadyPending += reqSkipAlreadyPending;
    diag.reqSkipEmptyTile += reqSkipEmptyTile;
    diag.reqSkipClassified += reqSkipClassified;
    diag.reqSkipUpsampleSrc += reqSkipUpsampleSrc;
    diag.reqSkipUpsampleNoContent += reqSkipUpsampleNoContent;
    diag.reqSkipDispatch += reqSkipDispatch;
    diag.reqSkipNoProvider += reqSkipNoProvider;
    diag.reqStopDispatch += reqStopDispatch;
    diag.rasterOverlayTilesLoading += rasterOverlayTilesLoading;
    diag.rasterSourceRequestsInFlight += rasterSourceRequestsInFlight;
    diag.rasterPendingUploads += rasterPendingUploads;
    diag.rasterPendingUploadBytes = saturatingAddInt(
        diag.rasterPendingUploadBytes,
        rasterPendingUploadBytes);
    diag.rasterCachedSourceTileBytes = saturatingAddInt(
        diag.rasterCachedSourceTileBytes,
        rasterCachedSourceTileBytes);
    diag.frameMappedRasterTileCount += frameMappedRasterTileCount;
    diag.frameMappedRasterTileLoadingCount +=
        frameMappedRasterTileLoadingCount;
    diag.frameProgressTotalCount += frameProgressTotalCount;
    diag.frameProgressLoadingCount += frameProgressLoadingCount;
    diag.frameLoadProgressPercentage =
        diag.frameProgressLoadingCount == 0 ||
                diag.frameProgressTotalCount <= 0
            ? 100.0
            : 100.0 *
                  static_cast<double>(diag.frameProgressTotalCount -
                                      diag.frameProgressLoadingCount) /
                  static_cast<double>(diag.frameProgressTotalCount);
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
    diag.pendingGltfTerrainUploads = 0;
    diag.pendingGltfTerrainTerminalResults = 0;
    diag.pendingContentRequests = 0;
    diag.pendingContentUploads = 0;
    diag.pendingContentTerminalResults = 0;
    diag.requestIssued = 0;
    diag.requestBlockedByInflight = 0;
    diag.reqSkipEmptyKey = 0;
    diag.reqSkipAlreadyPending = 0;
    diag.reqSkipEmptyTile = 0;
    diag.reqSkipClassified = 0;
    diag.reqSkipUpsampleSrc = 0;
    diag.reqSkipUpsampleNoContent = 0;
    diag.reqSkipDispatch = 0;
    diag.reqSkipNoProvider = 0;
    diag.reqStopDispatch = 0;
    resetProviderDiagnostics(diag);
    diag.rasterOverlayTilesLoading = 0;
    diag.rasterSourceRequestsInFlight = 0;
    diag.rasterPendingUploads = 0;
    diag.rasterPendingUploadBytes = 0;
    diag.rasterCachedSourceTileBytes = 0;
    diag.frameMappedRasterTileCount = 0;
    diag.frameMappedRasterTileLoadingCount = 0;
    diag.frameProgressTotalCount = 0;
    diag.frameProgressLoadingCount = 0;
    diag.frameLoadProgressPercentage = 100.0;
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
