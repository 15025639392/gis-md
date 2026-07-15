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
        input.deltaSeconds,
        [&input](double deltaSeconds) {
            TileLodTransitionFrameUpdater::update(
                input.tilePlan,
                input.tileRegistry,
                input.activeTiles,
                input.previousActiveTiles,
                input.fadingKeys,
                input.rasterOverlays,
                deltaSeconds,
                input.lodTransitionOptions);
        },
        [&input]() {
            TileRenderPlanFrameRefresher::refresh(
                input.tilePlan,
                input.contentAccess,
                input.rasterOverlays,
                input.renderPlanOptions);
        },
        [&input](const TilesetTile& tile) {
            return TileSelectionRasterOverlayPreparer::isRenderable(
                tile,
                input.rasterOverlays);
        });
}

} // namespace earth_engine
