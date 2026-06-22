#include "TileTerminalLoadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

namespace {

void markUnknownTemporaryFailure(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
    tile.markContentFailedTemporarily();
}

void markUnknownPermanentFailure(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
    tile.markContentFailedPermanently();
}

void applyNativeEmptyContentRefinement(TilesetTile& tile) {
    const TilesetTile* ancestor = tile.parent;
    while (ancestor && ancestor->unconditionallyRefine) {
        ancestor = ancestor->parent;
    }
    const double tileError = tile.nonZeroGeometricError();
    const double parentError = ancestor
        ? ancestor->nonZeroGeometricError()
        : tileError * 2.0;
    if (tileError >= parentError) {
        tile.unconditionallyRefine = true;
    }
}

} // namespace

TileTerminalLoadAction
TileTerminalLoadPolicy::applyTerrainTerminalResult(
    TilesetTile& tile,
    TileLoadStatus status,
    IPrepareRendererResources* pPrepRenderer) {
    TileTerminalLoadAction action;

    switch (status) {
        case TileLoadStatus::Empty: {
            action.markEmptyCacheKey = true;
            tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
            tile.markEmptyContentLoaded();
            applyNativeEmptyContentRefinement(tile);
            tile.markEmptyContentDone();
            action.resourcesDirty = true;
            break;
        }
        case TileLoadStatus::RetryLater:
        case TileLoadStatus::Cancelled:
            markUnknownTemporaryFailure(tile, pPrepRenderer);
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Failed:
        case TileLoadStatus::Renderable:
        case TileLoadStatus::External:
            markUnknownPermanentFailure(tile, pPrepRenderer);
            action.resourcesDirty = true;
            break;
    }

    return action;
}

TileTerminalLoadAction
TileTerminalLoadPolicy::applyContentTerminalResult(
    TilesetTile& tile,
    TileLoadStatus status,
    IPrepareRendererResources* pPrepRenderer) {
    TileTerminalLoadAction action;

    switch (status) {
        case TileLoadStatus::Empty:
            action.markEmptyCacheKey = true;
            tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
            applyNativeEmptyContentRefinement(tile);
            tile.markEmptyContentDone();
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::External:
            tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
            tile.markExternalContentDone();
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::RetryLater:
            markUnknownTemporaryFailure(tile, pPrepRenderer);
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Cancelled:
            markUnknownTemporaryFailure(tile, pPrepRenderer);
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Failed:
            markUnknownPermanentFailure(tile, pPrepRenderer);
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Renderable:
            markUnknownPermanentFailure(tile, pPrepRenderer);
            action.resourcesDirty = true;
            break;
    }

    return action;
}

} // namespace earth_engine
