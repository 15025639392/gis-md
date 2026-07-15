#include "RenderContentRasterOverlayStateUpdater.h"

#include "TileRasterOverlayPrefetcher.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../renderer/Renderer.h"

namespace earth_engine {

RenderContentRasterOverlayUpdateAction
RenderContentRasterOverlayStateUpdater::update(
    Renderer& renderer,
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const std::vector<size_t>& overlayProcessingOrder,
    RenderDevice* device,
    double maximumScreenSpaceError,
    FrameResourceBudget& frameResourceBudget,
    uint64_t frameNumber) {
    return TileRasterOverlayPrefetcher::prefetch(
        tile,
        rasterOverlays,
        overlayProcessingOrder,
        device,
        maximumScreenSpaceError,
        frameResourceBudget,
        &renderer,
        frameNumber);
}

} // namespace earth_engine
