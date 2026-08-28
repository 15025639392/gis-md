#include "TileSelectionFrameFinalizationRunner.h"

#include "TileContentAccess.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TilesetTileRegistry.h"

namespace earth_engine {

TileSelectionFrameFinalizeTimings
TileSelectionFrameFinalizationRunner::finalize(
    TileSelectionFrameFinalizationInput input) {
    return TileSelectionFrameFinalizer::finalize(
        input.tilePlan,
        input.activeTiles,
        input.selectionCounters,
        [&input]() {
            TileRenderPlanFrameRefresher::refresh(
                input.tilePlan,
                input.contentAccess,
                input.configuredRasterOverlays,
                input.renderPlanOptions);
        },
        [&input](const TilesetTile& tile) {
            return TileSelectionRasterOverlayPreparer::isRenderable(
                tile,
                input.renderPlanOptions.rasterFrame);
        });
}

} // namespace earth_engine
