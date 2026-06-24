#include "TileRasterOverlayReadinessPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TileLoadState.h"
#include "TilesetTile.h"

#include "../layers/ActivatedRasterOverlay.h"
#include "../layers/RasterOverlay.h"

#include <algorithm>

namespace earth_engine {

bool TileRasterOverlayReadinessPolicy::doneTileCannotHoldRasterOverlays(
    const TilesetTile& tile) {
    return tile.content.loadState == TileLoadState::Done &&
           (tile.content.contentKind != TileContentKind::Render ||
            !tile.hasRasterOverlayHostContent());
}

bool TileRasterOverlayReadinessPolicy::requiredOverlaysReady(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        const ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible() ||
            !activeOverlay->getOverlay().blocksCompleteRenderable()) {
            continue;
        }
        if (!tile.rasterOverlayState.hasReadyMapping(i)) {
            return false;
        }
    }

    return true;
}

std::vector<size_t> TileRasterOverlayReadinessPolicy::processingOrder(
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    std::vector<size_t> order;
    order.reserve(rasterOverlays.size());
    for (size_t i = 0; i < rasterOverlays.size(); ++i) {
        order.push_back(i);
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const ActivatedRasterOverlay* lhs = rasterOverlays[a];
        const ActivatedRasterOverlay* rhs = rasterOverlays[b];
        const int lhsPriority = lhs
            ? static_cast<int>(lhs->getOverlay().priority())
            : static_cast<int>(RasterOverlayPriority::Low);
        const int rhsPriority = rhs
            ? static_cast<int>(rhs->getOverlay().priority())
            : static_cast<int>(RasterOverlayPriority::Low);
        return lhsPriority > rhsPriority;
    });
    return order;
}

} // namespace earth_engine
