#include "TileSelectionTraversalDetailsBuilder.h"

#include "DirectRasterMapping.h"
#include "TileSelectionHistory.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TilesetTile.h"

namespace earth_engine {

namespace {

bool wasRenderedLastFrameForTraversalDetails(const TilesetTile& tile) {
    return TileTraversalDetailsPolicy::wasRenderedLastFrameForTraversalDetails(
        tile.selectionFrameState.previousSelectionState,
        tile.refine,
        TileSelectionHistory::anyDescendantWasRenderedLastFrame(tile));
}

} // namespace

TileTraversalDetails TileSelectionTraversalDetailsBuilder::forSingleTile(
    const TilesetTile& tile,
    const RasterOverlayFrameContext& frame) {
    const bool renderable = TileSelectionRasterOverlayPreparer::isRenderable(
        tile,
        frame);

    return TileTraversalDetailsPolicy::forSingleTile(
        renderable,
        wasRenderedLastFrameForTraversalDetails(tile));
}

TileTraversalDetails TileSelectionTraversalDetailsBuilder::forCulledTile(
    const TilesetTile& tile,
    bool forbidHoles,
    const RasterOverlayFrameContext& frame) {
    if (!forbidHoles || tile.refine != TileRefine::Replace) {
        return TileTraversalDetails{};
    }

    const bool renderable = TileSelectionRasterOverlayPreparer::isRenderable(
        tile, frame);
    return TileTraversalDetailsPolicy::forCulledTile(
        forbidHoles,
        tile.refine,
        renderable,
        wasRenderedLastFrameForTraversalDetails(tile));
}

} // namespace earth_engine
