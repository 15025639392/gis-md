#include "TileSelectionStateResetter.h"

#include "DirectRasterMapping.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TileSelectionResetPolicy.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileSelectionStateResetter::resetOne(
    TilesetTile& tile,
    const RasterOverlayFrameContext& frame) {
    TileSelectionFrameState& selection = tile.selectionFrameState;
    const TileSelectionResetPlan resetPlan =
        TileSelectionResetPolicy::plan(
            TileSelectionResetInput{
                selection.selectionState,
                tile.hasSurfaceDrawable(),
                TileSelectionRasterOverlayPreparer::isCompleteRenderable(
                    tile,
                    frame)});
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
