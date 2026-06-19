#include "TileSelectionTraversalDetailsBuilder.h"

#include "RasterMappedToTilesetTile.h"
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
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    const bool renderable = TileSelectionRasterOverlayPreparer::isRenderable(
        tile,
        rasterOverlays);

    return TileTraversalDetailsPolicy::forSingleTile(
        renderable,
        wasRenderedLastFrameForTraversalDetails(tile));
}

TileTraversalDetails TileSelectionTraversalDetailsBuilder::forCulledTile(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    bool forbidHoles) {
    if (!forbidHoles || tile.refine != TileRefine::Replace) {
        return TileTraversalDetails{};
    }

    const bool renderable = TileSelectionRasterOverlayPreparer::isRenderable(
        tile,
        rasterOverlays);
    return TileTraversalDetailsPolicy::forCulledTile(
        forbidHoles,
        tile.refine,
        renderable,
        wasRenderedLastFrameForTraversalDetails(tile));
}

} // namespace earth_engine
