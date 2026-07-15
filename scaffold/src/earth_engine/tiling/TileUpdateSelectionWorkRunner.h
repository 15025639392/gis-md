#pragma once

#include "TileLoadQueue.h"
#include "TileLoadTypes.h"
#include "TilePlan.h"
#include "TileRasterOverlayFrameProcessor.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileSelectionCounters.h"
#include "TileFillProxyPreparer.h"
#include "TileSelectionPerformanceTimings.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TileSelectionReuseState.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../renderer/RenderDevice.h"
#include "../scene/FrameState.h"
#include "../debug/PerfTimer.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace earth_engine {

class IPrepareRendererResources;

struct TileUpdateSelectionWorkInput {
    TilePlan& tilePlan;
    TileLoadQueue& loadQueue;
    TileSelectionCounters& selectionCounters;
    TileSelectionReuseState& selectionReuseState;
    std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    FrameResourceBudget& frameResourceBudget;
    RenderDevice* device = nullptr;
    const FrameState& frameState;
    uint64_t currentResourceRevision = 0;
    uint64_t currentOverlaySignature = 0;
    TileSelectionReuseMode reuseMode = TileSelectionReuseMode::None;
    TileSelectionReuseRejectReason reuseRejectReason =
        TileSelectionReuseRejectReason::None;
    bool reusedSelection = false;
    double maximumScreenSpaceError = 16.0;
    bool enableTerrainFillProxy = false;
    int terrainFillProxyGridSize = 16;
    bool hasTerrainQuadtree = false;
    IPrepareRendererResources* pPrepRenderer = nullptr;
};

struct TileUpdateSelectionWorkResult {
    double computeMs = 0.0;
    double selectorTraversalMs = 0.0;
    double selectorRefineMs = 0.0;
    double selectorRenderPlanMs = 0.0;
    double selectorRequestPlanningMs = 0.0;
    double selectorRefineOverlayMs = 0.0;
    double selectorRefineDecisionMs = 0.0;
    double selectorRefineMaterializeMs = 0.0;
    double selectorRefineCommitMs = 0.0;
    int selectorRefineMaterializeCalls = 0;
    int selectorRefineMaterializeChanged = 0;
    int selectorRefineMaterializeRetry = 0;
    int selectorRefineMaterializeFastPath = 0;
    double selectorVisitVisibilityMs = 0.0;
    double selectorVisitInputMetricsMs = 0.0;
    double selectorVisitPolicyMs = 0.0;
    double prefetchMs = 0.0;
    double prefetchBaseMs = 0.0;
    double prefetchFillMs = 0.0;
    double prefetchRenderPlanMs = 0.0;
    double prefetchRenderPlanUpdateMs = 0.0;
    double prefetchRenderPlanActionMs = 0.0;
    double prefetchVisibleMs = 0.0;
    double prefetchLoadQueueMs = 0.0;
    double prefetchAdvanceMs = 0.0;
    double prefetchMapMs = 0.0;
    int prefetchVisibleTiles = 0;
    int prefetchLoadQueueTiles = 0;
    int prefetchAdvanceCount = 0;
    int prefetchMapCount = 0;
    int prefetchRenderPlanTiles = 0;
    int prefetchRenderPlanAuthoritativeUpdates = 0;
    int prefetchRenderPlanStableReuses = 0;
    double requestMs = 0.0;
    TileSelectionReuseMode reuseMode = TileSelectionReuseMode::None;
    TileSelectionReuseRejectReason reuseRejectReason =
        TileSelectionReuseRejectReason::None;
    bool reusedSelection = false;
};

class TileUpdateSelectionWorkRunner {
public:
    template <typename RefreshTilePlanRenderEntriesFn,
              typename SelectTilesFn,
              typename FindTileFn,
              typename EnsureTileFn,
              typename UnloadTileContentFn,
              typename CreateRasterOverlayUpsampledChildrenFn,
              typename RequestMissingTilesFn,
              typename ReconcileTileResourcesFn>
    static TileUpdateSelectionWorkResult run(
        TileUpdateSelectionWorkInput input,
        RefreshTilePlanRenderEntriesFn&& refreshTilePlanRenderEntries,
        SelectTilesFn&& selectTiles,
        FindTileFn&& findTile,
        EnsureTileFn&& ensureTile,
        UnloadTileContentFn&& unloadTileContent,
        CreateRasterOverlayUpsampledChildrenFn&&
            createRasterOverlayUpsampledChildren,
        RequestMissingTilesFn&& requestMissingTiles,
        ReconcileTileResourcesFn&& reconcileTileResources) {
        TileUpdateSelectionWorkResult result;
        result.reuseMode = input.reuseMode;
        result.reuseRejectReason = input.reuseRejectReason;
        result.reusedSelection =
            input.reuseMode != TileSelectionReuseMode::None ||
            input.reusedSelection;

        const double computeStartMs = perf::nowMs();
        if (input.reusedSelection) {
            input.tilePlan.frameId = input.frameState.frameId;
            input.selectionCounters.reset();
            // Strict reuse already proves that the selector views, resource
            // revision, overlay configuration, and fade state are unchanged.
            // Rebuilding render entries, credits, and progress would only
            // rescan the same tile set. Stale reuse is different: it may span
            // a resource revision while the camera is still settling, so it
            // must refresh the render-facing result.
            if (input.reuseMode != TileSelectionReuseMode::Strict) {
                refreshTilePlanRenderEntries();
            }
        } else {
            TileSelectionPerformanceTimings selectionTimings;
            selectTiles(input.frameState, selectionTimings);
            result.selectorTraversalMs = selectionTimings.traversalMs;
            result.selectorRefineMs = selectionTimings.refineMs;
            result.selectorRenderPlanMs = selectionTimings.renderPlanMs;
            result.selectorRefineOverlayMs =
                selectionTimings.refineOverlayMs;
            result.selectorRefineDecisionMs =
                selectionTimings.refineDecisionMs;
            result.selectorRefineMaterializeMs =
                selectionTimings.refineMaterializeMs;
            result.selectorRefineCommitMs =
                selectionTimings.refineCommitMs;
            result.selectorRefineMaterializeCalls =
                selectionTimings.refineMaterializeCalls;
            result.selectorRefineMaterializeChanged =
                selectionTimings.refineMaterializeChanged;
            result.selectorRefineMaterializeRetry =
                selectionTimings.refineMaterializeRetry;
            result.selectorRefineMaterializeFastPath =
                selectionTimings.refineMaterializeFastPath;
            result.selectorVisitVisibilityMs =
                selectionTimings.visitVisibilityMs;
            result.selectorVisitInputMetricsMs =
                selectionTimings.visitInputMetricsMs;
            result.selectorVisitPolicyMs =
                selectionTimings.visitPolicyMs;
            input.selectionReuseState.commit(
                input.frameState,
                input.currentResourceRevision,
                input.currentOverlaySignature);
        }
        result.computeMs = perf::nowMs() - computeStartMs;

        // During reuse, the tile plan hasn't changed so rerunning the full
        // overlay prefetch is redundant.  Individual tile overlay states
        // (missing projections, failed tiles) are handled lazily: they will
        // be detected on the next non-reuse frame's prefetch or by the
        // overlay provider's incremental tile update system.  Matching
        // cesium-native where updateTileOverlays runs outside selection.
        if (!input.reusedSelection) {
            const double prefetchStartMs = perf::nowMs();
            const std::vector<size_t> overlayOrder =
                TileSelectionRasterOverlayPreparer::processingOrder(
                    input.rasterOverlays);
            const TileRasterOverlayPrefetchResult prefetchResult =
                TileRasterOverlayFrameProcessor::prefetchSelection(
                    input.tilePlan,
                    input.loadQueue.requests(),
                    input.rasterOverlays,
                    overlayOrder,
                    input.device,
                    input.maximumScreenSpaceError,
                    input.frameResourceBudget,
                    ensureTile,
                    input.pPrepRenderer,
                    std::forward<UnloadTileContentFn>(unloadTileContent),
                    [&input](const TileKey& key,
                             TileLoadPriorityGroup group,
                             double priority) {
                        input.loadQueue.queue(key, group, priority);
                    });
            result.prefetchBaseMs = perf::nowMs() - prefetchStartMs;
            result.prefetchMs = result.prefetchBaseMs;
            result.prefetchVisibleMs = prefetchResult.visibleLoopMs;
            result.prefetchLoadQueueMs = prefetchResult.loadQueueLoopMs;
            result.prefetchAdvanceMs = prefetchResult.advanceLoadsMs;
            result.prefetchMapMs = prefetchResult.prefetchMappingsMs;
            result.prefetchVisibleTiles = prefetchResult.visibleTilesConsidered;
            result.prefetchLoadQueueTiles =
                prefetchResult.loadQueueTilesConsidered;
            result.prefetchAdvanceCount = prefetchResult.advanceLoadsCount;
            result.prefetchMapCount = prefetchResult.prefetchMappingsCount;
        }

        // Terrain fill proxy: give each visible tile still lacking real terrain
        // a drape-ready ellipsoid proxy so imagery appears on the smooth globe
        // immediately (cesium-js TerrainFillMesh model). Runs after prefetch
        // (imagery mappings advanced) and before refreshRenderEntries so
        // canBuildRenderEntryDirectly picks up freshly-ready fills this frame.
        // Flag-gated (default off) → golden/existing behavior unchanged.
        if (input.enableTerrainFillProxy && input.hasTerrainQuadtree) {
            const double fillStartMs = perf::nowMs();
            bool fillResourcesChanged = false;
            const std::vector<size_t> fillOverlayOrder =
                TileSelectionRasterOverlayPreparer::processingOrder(
                    input.rasterOverlays);
            for (const TileKey& key : input.tilePlan.visibleTiles) {
                TilesetTile* tile = findTile(key);
                if (!tile) {
                    tile = ensureTile(key);
                }
                if (!tile) continue;
                const TileFillProxyPrepareResult fillResult =
                    TileFillProxyPreparer::ensureFillProxy(
                        *tile,
                        input.device,
                        input.terrainFillProxyGridSize,
                        input.pPrepRenderer);
                if (fillResult.resourcesChanged) {
                    reconcileTileResources(*tile);
                    fillResourcesChanged = true;
                }
                // Map imagery onto a fill tile (keyed on its bounding-volume
                // rectangle — the proxy's own rectangle). The normal Done-path
                // raster update never runs for a still-loading fill tile.
                // In reuse frames we still need this narrow fill-only pump:
                // Strict reuse intentionally skips the expensive full-selection
                // prefetch, but already-visible fill proxies must keep their
                // throttled imagery requests moving.
                if (tile->content.renderContent.drawsFill() &&
                    !input.rasterOverlays.empty()) {
                    if (tile->rasterOverlayState.mappingCount() == 0) {
                        TileRasterOverlayPrefetcher::prefetch(
                            *tile,
                            input.rasterOverlays,
                            fillOverlayOrder,
                            input.device,
                            input.maximumScreenSpaceError,
                            input.frameResourceBudget,
                            input.pPrepRenderer,
                            input.frameState.frameId);
                    } else if (input.reusedSelection) {
                        TileRasterOverlayPrefetcher::advanceThrottledLoads(
                            *tile,
                            input.rasterOverlays,
                            fillOverlayOrder,
                            input.device,
                            input.frameResourceBudget);
                    }
                }
            }
            if (fillResourcesChanged) {
                refreshTilePlanRenderEntries();
            }
            result.prefetchFillMs = perf::nowMs() - fillStartMs;
            result.prefetchMs += result.prefetchFillMs;
        }

        const std::vector<size_t> renderOverlayOrder =
            TileSelectionRasterOverlayPreparer::processingOrder(
                input.rasterOverlays);
        const TileRasterOverlayRenderPlanPrepareResult renderPlanRaster =
            TileRasterOverlayFrameProcessor::prepareRenderPlan(
                input.tilePlan,
                input.rasterOverlays,
                renderOverlayOrder,
                input.device,
                input.maximumScreenSpaceError,
                input.frameResourceBudget,
                input.pPrepRenderer,
                unloadTileContent,
                createRasterOverlayUpsampledChildren,
                [&input](const TileKey& key,
                         TileLoadPriorityGroup group,
                         double priority) {
                    input.loadQueue.queue(key, group, priority);
                },
                refreshTilePlanRenderEntries);
        result.prefetchRenderPlanMs = renderPlanRaster.totalMs;
        result.prefetchRenderPlanUpdateMs = renderPlanRaster.updateMs;
        result.prefetchRenderPlanActionMs = renderPlanRaster.actionMs;
        result.prefetchRenderPlanTiles = renderPlanRaster.tilesConsidered;
        result.prefetchRenderPlanAuthoritativeUpdates =
            renderPlanRaster.authoritativeUpdates;
        result.prefetchRenderPlanStableReuses =
            renderPlanRaster.stableReuses;
        result.prefetchMs += result.prefetchRenderPlanMs;

        const double requestStartMs = perf::nowMs();
        TileLoadRequestOutcome requestOutcome;
        requestOutcome = requestMissingTiles(
            input.loadQueue,
            &input.frameResourceBudget);
        input.selectionReuseState.recordRequestOutcome(
            requestOutcome.issued > 0,
            requestOutcome.blockedByInflight);
        result.requestMs = perf::nowMs() - requestStartMs;
        result.selectorRequestPlanningMs = result.requestMs;

        return result;
    }
};

} // namespace earth_engine
