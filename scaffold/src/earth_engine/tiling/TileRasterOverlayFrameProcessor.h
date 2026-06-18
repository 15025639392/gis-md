#pragma once

#include "TileLoadTypes.h"
#include "TilePlan.h"
#include "TileRasterOverlayPrefetcher.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"

#include <cstddef>
#include <vector>

namespace earth_engine {

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
        EnsureTileFn&& ensureTile) {
        for (const TileKey& key : tilePlan.visibleTiles) {
            if (TilesetTile* tile = ensureTile(key)) {
                TileRasterOverlayPrefetcher::prefetch(
                    *tile,
                    rasterOverlays,
                    overlayProcessingOrder,
                    device,
                    maximumScreenSpaceError,
                    frameResourceBudget);
            }
        }
        for (const TileLoadRequest& request : loadRequests) {
            if (TilesetTile* tile = ensureTile(request.key)) {
                TileRasterOverlayPrefetcher::prefetch(
                    *tile,
                    rasterOverlays,
                    overlayProcessingOrder,
                    device,
                    maximumScreenSpaceError,
                    frameResourceBudget);
            }
        }
    }
};

} // namespace earth_engine
