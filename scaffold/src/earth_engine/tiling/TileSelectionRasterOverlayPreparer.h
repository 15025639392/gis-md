#pragma once

#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "RasterMappedToTilesetTile.h"
#include "TileRasterOverlayPrefetcher.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileRenderablePolicy.h"
#include "TilesetTile.h"

#include <cstddef>
#include <vector>

namespace earth_engine {

class FrameResourceBudget;
class RenderDevice;
struct TilesetTile;

class TileSelectionRasterOverlayPreparer {
public:
    static bool canSkipReadyOverlayPrefetch(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        if (!TileRasterOverlayReadinessPolicy::requiredOverlaysReady(
                tile,
                rasterOverlays)) {
            return false;
        }
        for (size_t i = 0;
             i < rasterOverlays.size() &&
             i < tile.rasterOverlayState.mappingCount();
             ++i) {
            const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
            if (!activeOverlay || !activeOverlay->visible() ||
                !activeOverlay->getOverlay().blocksCompleteRenderable()) {
                continue;
            }
            const RasterMappedToTilesetTile* mapped =
                tile.rasterOverlayState.mappingAt(i);
            if (mapped && mapped->isMoreDetailAvailable()) {
                return false;
            }
        }
        return true;
    }

    static bool isCompleteRenderable(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        const bool requiredRasterOverlaysReady =
            TileRasterOverlayReadinessPolicy::requiredOverlaysReady(
                tile,
                rasterOverlays);

        return TileRenderablePolicy::isCompleteRenderable(
            tile.renderableSnapshot(requiredRasterOverlaysReady));
    }

    static bool isRenderable(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        return isCompleteRenderable(tile, rasterOverlays);
    }

    static std::vector<size_t> processingOrder(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        return TileRasterOverlayReadinessPolicy::processingOrder(
            rasterOverlays);
    }

    static void prepare(
        TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        RenderDevice* device,
        double maximumScreenSpaceError,
        FrameResourceBudget& frameResourceBudget) {
        if (!tile.canPrepareRasterOverlays()) {
            return;
        }

        // Selection asks renderability before command building can update
        // raster mappings, so advance required overlays during traversal.
        if (!rasterOverlays.empty() &&
            tile.rasterOverlayState.mappingCount() >= rasterOverlays.size()) {
            if (canSkipReadyOverlayPrefetch(
                    tile,
                    rasterOverlays)) {
                return;
            }
        }

        const std::vector<size_t> overlayOrder = processingOrder(
            rasterOverlays);
        TileRasterOverlayPrefetcher::prefetch(
            tile,
            rasterOverlays,
            overlayOrder,
            device,
            maximumScreenSpaceError,
            frameResourceBudget);
    }
};

} // namespace earth_engine
