#pragma once

#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"
#include "DirectRasterMapping.h"
#include "TileRasterOverlayPrefetcher.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "RasterOverlayRuntime.h"
#include "TileRasterOverlaySignature.h"
#include "TileRenderablePolicy.h"
#include "TilesetTile.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace earth_engine {

class FrameResourceBudget;
class RenderDevice;
struct TilesetTile;

class TileSelectionRasterOverlayPreparer {
public:
    static bool canSkipReadyOverlayPrefetch(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame) {
        const auto& rasterOverlays = frame.directOverlays();
        if (!TileRasterOverlayReadinessPolicy::requiredOverlaysReady(
                tile,
                frame)) {
            return false;
        }
        for (size_t i = 0;
             i < rasterOverlays.size() &&
             i < tile.rasterOverlayState.mappingCount();
            ++i) {
            const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
            const RasterOverlayFrameSlot& slot = frame.slots()[i];
            const bool visible =
                slot.directProvider != nullptr && slot.visible;
            const bool blocks = slot.blocksCompleteRenderable;
            if (!activeOverlay || !visible || !blocks) {
                continue;
            }
            const DirectRasterMapping* mapped =
                tile.rasterOverlayState.mappingAt(i);
            if (mapped && mapped->hasPendingNonPlaceholderLoadingTile()) {
                return false;
            }
            if (mapped && mapped->isMoreDetailAvailable()) {
                return false;
            }
        }
        return true;
    }

    static bool isCompleteRenderable(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame) {
        const bool requiredRasterOverlaysReady =
            TileRasterOverlayReadinessPolicy::requiredOverlaysReady(
                tile,
                frame);

        return TileRenderablePolicy::isCompleteRenderable(
            tile.renderableSnapshot(requiredRasterOverlaysReady));
    }

    static bool isRenderable(
        const TilesetTile& tile,
        const RasterOverlayFrameContext& frame) {
        return isCompleteRenderable(tile, frame);
    }

    static std::vector<size_t> processingOrder(
        const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
        return TileRasterOverlayReadinessPolicy::processingOrder(
            rasterOverlays);
    }

    static void prepare(
        TilesetTile& tile,
        const RasterOverlayFrameContext& frame,
        RenderDevice* device,
        double maximumScreenSpaceError,
        FrameResourceBudget& frameResourceBudget,
        uint64_t frameNumber = 0,
        IPrepareRendererResources* pPrepRenderer = nullptr) {
        const auto& rasterOverlays = frame.directOverlays();
        if (!tile.canPrepareRasterOverlays()) {
            return;
        }

        // Selection needs current raster readiness before it can classify the
        // tile. Advance the update-owned mapping state here; command building
        // only consumes the prepared result.
        if (!rasterOverlays.empty() &&
            tile.rasterOverlayState.mappingCount() >= rasterOverlays.size()) {
            if (canSkipReadyOverlayPrefetch(
                    tile,
                    frame)) {
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
            frameResourceBudget,
            pPrepRenderer,
            frameNumber);
    }
};

} // namespace earth_engine
