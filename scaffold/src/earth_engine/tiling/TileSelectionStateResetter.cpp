#include "TileSelectionStateResetter.h"

#include "RasterMappedToTilesetTile.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TileSelectionResetPolicy.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileSelectionStateResetter::resetOne(
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    TileSelectionFrameState& selection = tile.selectionFrameState;
    const TileSelectionResetPlan resetPlan =
        TileSelectionResetPolicy::plan(
            TileSelectionResetInput{
                selection.selectionState,
                tile.hasSurfaceDrawable(),
                TileSelectionRasterOverlayPreparer::isCompleteRenderable(
                    tile,
                    rasterOverlays)});
    selection.previousSelectionState = resetPlan.previousSelectionState;
    selection.selectionState = resetPlan.selectionState;
    selection.screenSpaceError = resetPlan.screenSpaceError;
    selection.inFrustum = resetPlan.inFrustum;
    selection.cameraInside = resetPlan.cameraInside;
    selection.ancestorMeetsSse = resetPlan.ancestorMeetsSse;
    tile.updateFrameRenderability(
        resetPlan.surfaceDrawable,
        resetPlan.completeRenderable);
}

} // namespace earth_engine
