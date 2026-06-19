#include "TileSelectionStateResetter.h"

#include "RasterMappedToTilesetTile.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TileSelectionResetPolicy.h"
#include "TilesetTileRegistry.h"

namespace earth_engine {

void TileSelectionStateResetter::reset(
    TilesetTileRegistry& tileRegistry,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    for (auto& [ck, tile] : tileRegistry.tiles()) {
        if (!tile) continue;
        TileSelectionFrameState& selection = tile->selectionFrameState;
        const TileSelectionResetPlan resetPlan =
            TileSelectionResetPolicy::plan(
                TileSelectionResetInput{
                    selection.selectionState,
                    tile->hasSurfaceDrawable(),
                    TileSelectionRasterOverlayPreparer::isCompleteRenderable(
                        *tile,
                        rasterOverlays)});
        selection.previousSelectionState = resetPlan.previousSelectionState;
        selection.selectionState = resetPlan.selectionState;
        selection.screenSpaceError = resetPlan.screenSpaceError;
        selection.inFrustum = resetPlan.inFrustum;
        selection.cameraInside = resetPlan.cameraInside;
        selection.ancestorMeetsSse = resetPlan.ancestorMeetsSse;
        tile->updateFrameRenderability(
            resetPlan.surfaceDrawable,
            resetPlan.completeRenderable);
        (void)ck;
    }
}

} // namespace earth_engine
