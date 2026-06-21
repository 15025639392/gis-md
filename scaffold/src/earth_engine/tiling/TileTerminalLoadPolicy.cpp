#include "TileTerminalLoadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

namespace {

void markUnknownTemporaryFailure(TilesetTile& tile) {
    tile.rasterOverlayState.mappings().clear();
    tile.markContentFailedTemporarily();
}

void markUnknownPermanentFailure(TilesetTile& tile) {
    tile.rasterOverlayState.mappings().clear();
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
    TileLoadStatus status) {
    TileTerminalLoadAction action;

    switch (status) {
        case TileLoadStatus::Empty: {
            action.markEmptyCacheKey = true;
            tile.rasterOverlayState.mappings().clear();
            tile.markEmptyContentLoaded();
            applyNativeEmptyContentRefinement(tile);
            tile.markEmptyContentDone();
            action.resourcesDirty = true;
            break;
        }
        case TileLoadStatus::RetryLater:
        case TileLoadStatus::Cancelled:
            markUnknownTemporaryFailure(tile);
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Failed:
        case TileLoadStatus::Renderable:
        case TileLoadStatus::External:
            markUnknownPermanentFailure(tile);
            action.resourcesDirty = true;
            break;
    }

    return action;
}

TileTerminalLoadAction
TileTerminalLoadPolicy::applyContentTerminalResult(
    TilesetTile& tile,
    TileLoadStatus status) {
    TileTerminalLoadAction action;

    switch (status) {
        case TileLoadStatus::Empty:
            action.markEmptyCacheKey = true;
            tile.rasterOverlayState.mappings().clear();
            applyNativeEmptyContentRefinement(tile);
            tile.markEmptyContentDone();
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::External:
            tile.rasterOverlayState.mappings().clear();
            tile.markExternalContentDone();
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::RetryLater:
            markUnknownTemporaryFailure(tile);
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Cancelled:
            markUnknownTemporaryFailure(tile);
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Failed:
            markUnknownPermanentFailure(tile);
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Renderable:
            markUnknownPermanentFailure(tile);
            action.resourcesDirty = true;
            break;
    }

    return action;
}

} // namespace earth_engine
