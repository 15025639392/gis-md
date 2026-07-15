#pragma once

#include "TileSelectionCounters.h"
#include "TileSelectionReusePolicy.h"

#include <cstddef>

namespace earth_engine {

struct TileUpdateDebugLogInput {
    size_t renderTileCount = 0;
    size_t loadRequestCount = 0;
    double selectorMs = 0.0;
    double selectorTraversalMs = 0.0;
    double selectorRefineMs = 0.0;
    double selectorRenderPlanMs = 0.0;
    double selectorRequestPlanningMs = 0.0;
    double prefetchMs = 0.0;
    double prefetchBaseMs = 0.0;
    double prefetchFillMs = 0.0;
    double prefetchVisibleMs = 0.0;
    double prefetchLoadQueueMs = 0.0;
    double prefetchAdvanceMs = 0.0;
    double prefetchMapMs = 0.0;
    double requestMs = 0.0;
    double terrainUploadMs = 0.0;
    double rasterUploadMs = 0.0;
    double rasterSelectTaskMs = 0.0;
    double rasterUploadTextureMs = 0.0;
    double rasterTileFinalizeMs = 0.0;
    double rasterBookkeepingMs = 0.0;
    size_t terrainCacheSize = 0;
    size_t pendingRequestCount = 0;
    int rasterSourceRequestsInFlight = 0;
    int rasterActiveMappedSourceSets = 0;
    int rasterPendingSourceFallbacks = 0;
    int rasterInFlightSourceTiles = 0;
    int rasterInFlightSourceWaiters = 0;
    int rasterPendingUploads = 0;
    TileSelectionCounters selectionCounters;
    TileSelectionReuseMode reuseMode = TileSelectionReuseMode::None;
    TileSelectionReuseRejectReason reuseRejectReason =
        TileSelectionReuseRejectReason::None;
    bool reusedSelection = false;
    int prefetchVisibleTiles = 0;
    int prefetchLoadQueueTiles = 0;
    int prefetchAdvanceCount = 0;
    int prefetchMapCount = 0;
    int rasterUploadsProcessed = 0;
    int rasterMappedUploadsProcessed = 0;
    double rasterUploadMaxMs = 0.0;
    int rasterUploadMaxWidth = 0;
    int rasterUploadMaxHeight = 0;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
    double selectorRefineOverlayMs = 0.0;
    double selectorRefineDecisionMs = 0.0;
    double selectorRefineMaterializeMs = 0.0;
    double selectorRefineCommitMs = 0.0;
    double rasterSourceFallbackMs = 0.0;
    double rasterSourceSnapshotMs = 0.0;
    double rasterSourceIssueMs = 0.0;
    double rasterUploadQueueSelectMs = 0.0;
    int selectorRefineMaterializeCalls = 0;
    int selectorRefineMaterializeChanged = 0;
    int selectorRefineMaterializeRetry = 0;
    int selectorRefineMaterializeFastPath = 0;
    double selectorVisitVisibilityMs = 0.0;
    double selectorVisitInputMetricsMs = 0.0;
    double selectorVisitPolicyMs = 0.0;
    double prefetchRenderPlanMs = 0.0;
    double prefetchRenderPlanUpdateMs = 0.0;
    double prefetchRenderPlanActionMs = 0.0;
    int prefetchRenderPlanTiles = 0;
    int prefetchRenderPlanAuthoritativeUpdates = 0;
    int prefetchRenderPlanStableReuses = 0;
};

} // namespace earth_engine
