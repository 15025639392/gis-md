#pragma once

#include "TileLoadTypes.h"
#include "TileLoadPriorityPolicy.h"
#include "TilePlan.h"
#include "TileRasterOverlayPrefetcher.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"

#include <cstddef>
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
        IPrepareRendererResources* pPrepRenderer = nullptr) {
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
                TileRasterOverlayPrefetcher::prefetch(
                    *item.tile,
                    rasterOverlays,
                    overlayProcessingOrder,
                    device,
                    maximumScreenSpaceError,
                    frameResourceBudget,
                    pPrepRenderer);
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
                TileRasterOverlayPrefetcher::prefetch(
                    *tile,
                    rasterOverlays,
                    overlayProcessingOrder,
                    device,
                    maximumScreenSpaceError,
                    frameResourceBudget,
                    pPrepRenderer);
            }
        }
    }
};

} // namespace earth_engine
