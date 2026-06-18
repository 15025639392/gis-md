#pragma once

#include "TileRasterOverlayPrefetcher.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileRenderablePolicy.h"
#include "TilesetTile.h"

#include <cstddef>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class FrameResourceBudget;
class RenderDevice;
struct TilesetTile;

class TileSelectionRasterOverlayPreparer {
public:
    static bool isCompleteRenderable(
        const TilesetTile& tile,
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        const bool requiredRasterOverlaysReady =
            TileRasterOverlayReadinessPolicy::requiredOverlaysReady(
                tile,
                rasterOverlays);

        return TileRenderablePolicy::isCompleteRenderable(
            TileRenderableSnapshot{
                tile.loadState,
                tile.contentKind,
                tile.unconditionallyRefine,
                !tile.children.empty(),
                requiredRasterOverlaysReady,
                tile.meshReady});
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
        if (tile.loadState != TileLoadState::Done ||
            tile.contentKind != TileContentKind::Render ||
            !tile.meshReady ||
            !tile.mesh) {
            return;
        }

        // Selection asks renderability before command building can update
        // raster mappings, so advance required overlays during traversal.
        if (!rasterOverlays.empty() &&
            tile.rasterOverlays.size() >= rasterOverlays.size()) {
            if (TileRasterOverlayReadinessPolicy::requiredOverlaysReady(
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
