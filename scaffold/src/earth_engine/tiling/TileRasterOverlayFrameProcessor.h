#pragma once

#include "TileLoadTypes.h"
#include "TileLoadPriorityPolicy.h"
#include "TilePlan.h"
#include "TileRasterOverlayPrefetcher.h"
#include "TileRasterOverlayUploadResult.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../debug/PerfTimer.h"
#include "../layers/ActivatedRasterOverlay.h"

#include <cstddef>
#include <functional>
#include <limits>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class IPrepareRendererResources;

struct TileRasterOverlayPrefetchResult {
    int visibleTilesConsidered = 0;
    int loadQueueTilesConsidered = 0;
    int advanceLoadsCount = 0;
    int prefetchMappingsCount = 0;
    int earlyMappingsCount = 0;
    int visibleEarlyMappingsCount = 0;
    int loadQueueEarlyMappingsCount = 0;
    bool earlyMappingBudgetExhausted = false;
    double visibleLoopMs = 0.0;
    double loadQueueLoopMs = 0.0;
    double advanceLoadsMs = 0.0;
    double prefetchMappingsMs = 0.0;
    double earlyMappingsMs = 0.0;
};

struct TileRasterOverlayRenderPlanPrepareResult {
    int tilesConsidered = 0;
    int authoritativeUpdates = 0;
    int stableReuses = 0;
    int unloadActions = 0;
    int upsampleActions = 0;
    int renderPlanRefreshes = 0;
    double totalMs = 0.0;
    double updateMs = 0.0;
    double actionMs = 0.0;
};

class TileRasterOverlayFrameProcessor {
public:
    static TileRasterOverlayUploadResult processPendingUploads(
        std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        bool interactionActive,
        FrameResourceBudget& frameResourceBudget) {
        TileRasterOverlayUploadResult result;
        for (auto* overlay : rasterOverlays) {
            if (overlay) {
                const TileRasterOverlayUploadResult overlayResult =
                    overlay->processPendingUploads(
                        interactionActive,
                        &frameResourceBudget);
                result.processedUploads += overlayResult.processedUploads;
                result.mappedUploads += overlayResult.mappedUploads;
                result.selectTaskMs += overlayResult.selectTaskMs;
                result.uploadTextureMs += overlayResult.uploadTextureMs;
                result.tileFinalizeMs += overlayResult.tileFinalizeMs;
                result.bookkeepingMs += overlayResult.bookkeepingMs;
                result.sourceFallbackMs += overlayResult.sourceFallbackMs;
                result.sourceSnapshotMs += overlayResult.sourceSnapshotMs;
                result.sourceIssueMs += overlayResult.sourceIssueMs;
                result.uploadQueueSelectMs +=
                    overlayResult.uploadQueueSelectMs;
                if (overlayResult.maxUploadMs > result.maxUploadMs) {
                    result.maxUploadMs = overlayResult.maxUploadMs;
                    result.maxUploadWidth = overlayResult.maxUploadWidth;
                    result.maxUploadHeight = overlayResult.maxUploadHeight;
                }
            }
        }
        result.resourcesDirty = result.processedUploads > 0;
        return result;
    }

    template <typename EnsureTileFn>
    static TileRasterOverlayPrefetchResult prefetchSelection(
        const TilePlan& tilePlan,
        const std::vector<TileLoadRequest>& loadRequests,
        std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        const std::vector<size_t>& overlayProcessingOrder,
        RenderDevice* device,
        double maximumScreenSpaceError,
        FrameResourceBudget& frameResourceBudget,
        EnsureTileFn&& ensureTile,
        IPrepareRendererResources* pPrepRenderer = nullptr,
        const std::function<void(TilesetTile&)>& unloadTileContent = {},
        const std::function<void(const TileKey&,
                                 TileLoadPriorityGroup,
                                 double)>& queueReload = {}) {
        TileRasterOverlayPrefetchResult result;
        struct PrefetchTile {
            TileKey key;
            TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
            double priority = std::numeric_limits<double>::max();
            TilesetTile* tile = nullptr;
        };
        const auto tryEarlyMapping =
            [&](TilesetTile& tile,
                const TileKey& key,
                TileLoadPriorityGroup group,
                double priority,
                bool fromLoadQueue) {
                const TileLoadState loadState = tile.content.loadState;
                const bool canMapBeforeContentDone =
                    loadState == TileLoadState::FailedTemporarily ||
                    loadState == TileLoadState::Unloaded ||
                    loadState == TileLoadState::ContentLoading;
                if (!canMapBeforeContentDone || rasterOverlays.empty()) {
                    return;
                }
                if (!frameResourceBudget.tryStartRasterOverlayMapping()) {
                    result.earlyMappingBudgetExhausted = true;
                    return;
                }
                const double earlyMappingStartMs = perf::nowMs();
                const TileRasterOverlayPrefetchAction action =
                    TileRasterOverlayPrefetcher::prefetch(
                        tile,
                        rasterOverlays,
                        overlayProcessingOrder,
                        device,
                        maximumScreenSpaceError,
                        frameResourceBudget,
                        pPrepRenderer,
                        tilePlan.frameId);
                const double earlyMappingElapsedMs =
                    perf::nowMs() - earlyMappingStartMs;
                frameResourceBudget.recordRasterOverlayMappingElapsed(
                    earlyMappingElapsedMs);
                result.earlyMappingsMs += earlyMappingElapsedMs;
                result.prefetchMappingsMs += earlyMappingElapsedMs;
                ++result.earlyMappingsCount;
                if (fromLoadQueue) {
                    ++result.loadQueueEarlyMappingsCount;
                } else {
                    ++result.visibleEarlyMappingsCount;
                }
                ++result.prefetchMappingsCount;
                if (action.unloadTileContent && unloadTileContent) {
                    unloadTileContent(tile);
                    if (queueReload) {
                        queueReload(key, group, priority);
                    }
                }
            };

        std::unordered_set<TileKey> prefetchedTiles;
        std::vector<PrefetchTile> visibleTiles;
        visibleTiles.reserve(tilePlan.visibleTiles.size());
        for (const TileKey& key : tilePlan.visibleTiles) {
            TilesetTile* tile = ensureTile(key);
            if (tile) {
                visibleTiles.push_back(PrefetchTile{
                    key,
                    TileLoadPriorityGroup::Normal,
                    tile->selectionFrameState.priority,
                    tile});
            }
        }
        TileLoadPriorityPolicy::sortByPriority(visibleTiles);
        const double visibleLoopStartMs = perf::nowMs();
        for (const PrefetchTile& item : visibleTiles) {
            if (item.tile) {
                ++result.visibleTilesConsidered;
                if (!prefetchedTiles.insert(item.key).second) {
                    continue;
                }
                // Geometry-to-raster mapping is a once-at-load step. Already
                // mapped tiles only need the cheap throttled request pump.
                if (item.tile->rasterOverlayState.mappingCount() > 0) {
                    if (item.tile->content.loadState != TileLoadState::Done) {
                        const double advanceStartMs = perf::nowMs();
                        TileRasterOverlayPrefetcher::advanceThrottledLoads(
                            *item.tile,
                            rasterOverlays,
                            overlayProcessingOrder,
                            device,
                            frameResourceBudget,
                            pPrepRenderer);
                        result.advanceLoadsMs +=
                            perf::nowMs() - advanceStartMs;
                        ++result.advanceLoadsCount;
                    }
                    continue;
                }
                if (item.tile->content.loadState != TileLoadState::Done) {
                    // Cesium Native establishes overlay mappings before
                    // starting geometry load. Restore that overlap only for
                    // priority-sorted visible tiles under an independent CPU
                    // budget.
                    tryEarlyMapping(
                        *item.tile,
                        item.key,
                        item.group,
                        item.priority,
                        false);
                    continue;
                }
                const double prefetchStartMs = perf::nowMs();
                const TileRasterOverlayPrefetchAction action =
                    TileRasterOverlayPrefetcher::prefetch(
                        *item.tile,
                        rasterOverlays,
                        overlayProcessingOrder,
                        device,
                        maximumScreenSpaceError,
                        frameResourceBudget,
                        pPrepRenderer,
                        tilePlan.frameId);
                result.prefetchMappingsMs += perf::nowMs() - prefetchStartMs;
                ++result.prefetchMappingsCount;
                if (action.unloadTileContent && unloadTileContent) {
                    unloadTileContent(*item.tile);
                    if (queueReload) {
                        queueReload(item.key, item.group, item.priority);
                    }
                    continue;
                }
            }
        }
        result.visibleLoopMs = perf::nowMs() - visibleLoopStartMs;
        std::unordered_set<TileKey> screenRefinementLoadKeys;
        screenRefinementLoadKeys.reserve(tilePlan.selectionRecords.size());
        if (tilePlan.frameId == frameResourceBudget.frameNumber()) {
            for (const TileSelectionRecord& record :
                 tilePlan.selectionRecords) {
                if (record.state ==
                        TileSelectionState::RenderedAndKicked &&
                    (record.inFrustum || record.cameraInside) &&
                    !record.ancestorMeetsSse) {
                    screenRefinementLoadKeys.insert(record.key);
                }
            }
        }
        std::vector<TileLoadRequest> sortedLoadRequests = loadRequests;
        TileLoadPriorityPolicy::sortByPriority(sortedLoadRequests);
        const double loadQueueLoopStartMs = perf::nowMs();
        for (const TileLoadRequest& request : sortedLoadRequests) {
            if (!frameResourceBudget.canIssue(
                    FrameResourceLane::RasterRequest,
                    TileLoadPriorityPolicy::toFramePriority(
                        request.group))) {
                break;
            }
            if (prefetchedTiles.count(request.key) > 0) {
                continue;
            }
            if (TilesetTile* tile = ensureTile(request.key)) {
                ++result.loadQueueTilesConsidered;
                if (!prefetchedTiles.insert(request.key).second) {
                    continue;
                }
                // Same 闸1 rule as the visible loop: already-mapped load-queue
                // tiles only advance throttled imagery loads; not-Done tiles are
                // NOT mapped here (mapping happens in update preparation once
                // the content reaches Done).
                // Load-queue tiles are by definition still loading, so this
                // removes their per-frame first-sighting mapping entirely.
                if (tile->rasterOverlayState.mappingCount() > 0) {
                    if (tile->content.loadState != TileLoadState::Done) {
                        const double advanceStartMs = perf::nowMs();
                        TileRasterOverlayPrefetcher::advanceThrottledLoads(
                            *tile,
                            rasterOverlays,
                            overlayProcessingOrder,
                            device,
                            frameResourceBudget);
                        result.advanceLoadsMs +=
                            perf::nowMs() - advanceStartMs;
                        ++result.advanceLoadsCount;
                    }
                    continue;
                }
                if (tile->content.loadState != TileLoadState::Done) {
                    // REPLACE refinement keeps the renderable parent visible
                    // while its screen-relevant children exist only in the
                    // load queue. Motion culling may defer their geometry
                    // requests for many frames, so start a bounded amount of
                    // imagery work here as well. Use the plan-owned selection
                    // snapshot because reconciled async selection does not
                    // copy every live TileSelectionFrameState field.
                    // Preload, continuity ancestors, and offscreen work remain
                    // gated to avoid rebuilding the old queue-wide mapping
                    // flood.
                    const bool screenRelevant =
                        request.group != TileLoadPriorityGroup::Preload &&
                        screenRefinementLoadKeys.count(request.key) > 0;
                    if (screenRelevant) {
                        tryEarlyMapping(
                            *tile,
                            request.key,
                            request.group,
                            request.priority,
                            true);
                    }
                    continue;
                }
                const double prefetchStartMs = perf::nowMs();
                const TileRasterOverlayPrefetchAction action =
                    TileRasterOverlayPrefetcher::prefetch(
                        *tile,
                        rasterOverlays,
                        overlayProcessingOrder,
                        device,
                        maximumScreenSpaceError,
                        frameResourceBudget,
                        pPrepRenderer,
                        tilePlan.frameId);
                result.prefetchMappingsMs += perf::nowMs() - prefetchStartMs;
                ++result.prefetchMappingsCount;
                if (action.unloadTileContent && unloadTileContent) {
                    unloadTileContent(*tile);
                    if (queueReload) {
                        queueReload(request.key,
                                    request.group,
                                    request.priority);
                    }
                    continue;
                }
            }
        }
        result.loadQueueLoopMs = perf::nowMs() - loadQueueLoopStartMs;
        return result;
    }

    template <typename UnloadTileContentFn,
              typename CreateRasterOverlayUpsampledChildrenFn,
              typename QueueReloadFn,
              typename RefreshRenderPlanFn>
    static TileRasterOverlayRenderPlanPrepareResult prepareRenderPlan(
        TilePlan& tilePlan,
        std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        const std::vector<size_t>& overlayProcessingOrder,
        RenderDevice* device,
        double maximumScreenSpaceError,
        FrameResourceBudget& frameResourceBudget,
        IPrepareRendererResources* pPrepRenderer,
        UnloadTileContentFn&& unloadTileContent,
        CreateRasterOverlayUpsampledChildrenFn&&
            createRasterOverlayUpsampledChildren,
        QueueReloadFn&& queueReload,
        RefreshRenderPlanFn&& refreshRenderPlan) {
        TileRasterOverlayRenderPlanPrepareResult result;
        const double totalStartMs = perf::nowMs();
        if (rasterOverlays.empty() || tilePlan.renderEntries.empty()) {
            result.totalMs = perf::nowMs() - totalStartMs;
            return result;
        }

        std::unordered_set<TilesetTile*> preparedTiles;
        preparedTiles.reserve(tilePlan.renderEntries.size() * 2);
        std::vector<TilesetTile*> pendingTiles;
        pendingTiles.reserve(tilePlan.renderEntries.size());

        while (true) {
            pendingTiles.clear();
            for (const TileRenderEntry& entry : tilePlan.renderEntries) {
                TilesetTile* tile = entry.renderTile;
                if (tile && preparedTiles.insert(tile).second) {
                    pendingTiles.push_back(tile);
                }
            }
            if (pendingTiles.empty()) {
                break;
            }

            bool renderPlanInvalidated = false;
            for (TilesetTile* tile : pendingTiles) {
                if (!tile || !tile->canPrepareRasterOverlays()) {
                    continue;
                }
                ++result.tilesConsidered;
                const uint64_t updateCountBefore =
                    tile->rasterOverlayState.authoritativeUpdateCount();
                const double updateStartMs = perf::nowMs();
                const TileRasterOverlayPrefetchAction action =
                    TileRasterOverlayPrefetcher::prefetch(
                        *tile,
                        rasterOverlays,
                        overlayProcessingOrder,
                        device,
                        maximumScreenSpaceError,
                        frameResourceBudget,
                        pPrepRenderer,
                        tilePlan.frameId);
                result.updateMs += perf::nowMs() - updateStartMs;
                const uint64_t updateCountAfter =
                    tile->rasterOverlayState.authoritativeUpdateCount();
                if (updateCountAfter > updateCountBefore) {
                    result.authoritativeUpdates += static_cast<int>(
                        updateCountAfter - updateCountBefore);
                } else {
                    ++result.stableReuses;
                }

                const double actionStartMs = perf::nowMs();
                if (action.unloadTileContent) {
                    unloadTileContent(*tile);
                    queueReload(
                        tile->key,
                        TileLoadPriorityGroup::Normal,
                        tile->selectionFrameState.priority);
                    ++result.unloadActions;
                    renderPlanInvalidated = true;
                } else if (
                    action.createRasterOverlayUpsampledChildren &&
                    tile->children.empty()) {
                    createRasterOverlayUpsampledChildren(*tile);
                    ++result.upsampleActions;
                }
                result.actionMs += perf::nowMs() - actionStartMs;
            }

            if (renderPlanInvalidated) {
                refreshRenderPlan();
                ++result.renderPlanRefreshes;
            }
        }

        result.totalMs = perf::nowMs() - totalStartMs;
        return result;
    }
};

} // namespace earth_engine
