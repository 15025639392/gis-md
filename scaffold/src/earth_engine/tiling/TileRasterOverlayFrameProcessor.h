#pragma once

#include "TileLoadTypes.h"
#include "TileLoadPriorityPolicy.h"
#include "TilePlan.h"
#include "TileRasterOverlayPrefetcher.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"

#include <cstddef>
#include <functional>
#include <limits>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class IPrepareRendererResources;

struct TileRasterOverlayUploadResult {
    int processedUploads = 0;
    bool resourcesDirty = false;
};

class TileRasterOverlayFrameProcessor {
public:
    static TileRasterOverlayUploadResult processPendingUploads(
        std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        bool interactionActive,
        FrameResourceBudget& frameResourceBudget) {
        int processedUploads = 0;
        for (auto* overlay : rasterOverlays) {
            if (overlay) {
                processedUploads += overlay->processPendingUploads(
                    interactionActive,
                    &frameResourceBudget);
            }
        }
        return TileRasterOverlayUploadResult{
            processedUploads,
            processedUploads > 0};
    }

    template <typename EnsureTileFn>
    static void prefetchSelection(
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
        struct PrefetchTile {
            TileKey key;
            TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal;
            double priority = std::numeric_limits<double>::max();
            TilesetTile* tile = nullptr;
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
        for (const PrefetchTile& item : visibleTiles) {
            if (item.tile) {
                if (!prefetchedTiles.insert(item.key).second) {
                    continue;
                }
                // cesium: the overlay geometry mapping is a once-at-load step,
                // NOT per-frame. 闸1: not-Done 瓦片一律不在 prefetch 建映射——
                // 映射交由几何加载完成(Done)后 render 路径的 RenderContent-
                // RasterOverlayStateUpdater 建立(cesium 里 updateTileOverlays
                // 只对已渲染 Done 瓦片)。这消除了拖动时对 not-Done 洪泛(数百
                // 个)每帧首见映射的 8-18ms 开销。已映射的(先前 Done 后回到
                // 加载中的)瓦片仅推进其 throttled 影像加载。
                if (item.tile->rasterOverlayState.mappingCount() > 0) {
                    if (item.tile->content.loadState != TileLoadState::Done) {
                        TileRasterOverlayPrefetcher::advanceThrottledLoads(
                            *item.tile,
                            rasterOverlays,
                            overlayProcessingOrder,
                            device,
                            frameResourceBudget);
                    }
                    continue;
                }
                if (item.tile->content.loadState != TileLoadState::Done) {
                    continue;
                }
                const TileRasterOverlayPrefetchAction action =
                    TileRasterOverlayPrefetcher::prefetch(
                    *item.tile,
                    rasterOverlays,
                    overlayProcessingOrder,
                    device,
                    maximumScreenSpaceError,
                    frameResourceBudget,
                    pPrepRenderer);
                if (action.unloadTileContent && unloadTileContent) {
                    unloadTileContent(*item.tile);
                    if (queueReload) {
                        queueReload(item.key, item.group, item.priority);
                    }
                    continue;
                }
            }
        }
        std::vector<TileLoadRequest> sortedLoadRequests = loadRequests;
        TileLoadPriorityPolicy::sortByPriority(sortedLoadRequests);
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
                if (!prefetchedTiles.insert(request.key).second) {
                    continue;
                }
                // Same 闸1 rule as the visible loop: already-mapped load-queue
                // tiles only advance throttled imagery loads; not-Done tiles are
                // NOT mapped here (mapping happens at Done via render path).
                // Load-queue tiles are by definition still loading, so this
                // removes their per-frame first-sighting mapping entirely.
                if (tile->rasterOverlayState.mappingCount() > 0) {
                    if (tile->content.loadState != TileLoadState::Done) {
                        TileRasterOverlayPrefetcher::advanceThrottledLoads(
                            *tile,
                            rasterOverlays,
                            overlayProcessingOrder,
                            device,
                            frameResourceBudget);
                    }
                    continue;
                }
                if (tile->content.loadState != TileLoadState::Done) {
                    continue;
                }
                const TileRasterOverlayPrefetchAction action =
                    TileRasterOverlayPrefetcher::prefetch(
                    *tile,
                    rasterOverlays,
                    overlayProcessingOrder,
                    device,
                    maximumScreenSpaceError,
                    frameResourceBudget,
                    pPrepRenderer);
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
    }
};

} // namespace earth_engine
