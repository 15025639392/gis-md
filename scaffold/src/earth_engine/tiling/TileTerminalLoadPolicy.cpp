#include "TileTerminalLoadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

namespace {

void markUnknownTemporaryFailure(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
    if (tile.content.contentKind == TileContentKind::Render) {
        tile.content.loadState = TileLoadState::FailedTemporarily;
    } else {
        tile.markContentFailedTemporarily();
    }
}

void markUnknownPermanentFailure(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
    tile.content.renderContent.clearRenderContent();
    tile.markContentFailedPermanently();
}

void clearRenderResidueForTerminalNonRenderContent(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
    tile.content.renderContent.clearRenderContent();
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

TileTerminalLoadAction applyTerminalResult(
    TilesetTile& tile,
    TileLoadStatus status,
    IPrepareRendererResources* pPrepRenderer,
    bool allowExternalContent) {
    TileTerminalLoadAction action;

    switch (status) {
        case TileLoadStatus::Empty:
            action.markEmptyCacheKey = true;
            clearRenderResidueForTerminalNonRenderContent(
                tile,
                pPrepRenderer);
            applyNativeEmptyContentRefinement(tile);
            tile.markEmptyContentDone();
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::External:
            if (allowExternalContent) {
                clearRenderResidueForTerminalNonRenderContent(
                    tile,
                    pPrepRenderer);
                tile.markExternalContentDone();
                action.ensureChildren = true;
            } else {
                markUnknownPermanentFailure(tile, pPrepRenderer);
            }
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::RetryLater:
        case TileLoadStatus::Cancelled:
            markUnknownTemporaryFailure(tile, pPrepRenderer);
            action.resourcesDirty = true;
            break;
        case TileLoadStatus::Failed:
        case TileLoadStatus::Renderable:
            markUnknownPermanentFailure(tile, pPrepRenderer);
            action.resourcesDirty = true;
            break;
    }

    return action;
}

} // namespace

TileTerminalLoadAction
TileTerminalLoadPolicy::applyTerrainTerminalResult(
    TilesetTile& tile,
    TileLoadStatus status,
    IPrepareRendererResources* pPrepRenderer) {
    return applyTerminalResult(tile, status, pPrepRenderer, false);
}

TileTerminalLoadAction
TileTerminalLoadPolicy::applyContentTerminalResult(
    TilesetTile& tile,
    TileLoadStatus status,
    IPrepareRendererResources* pPrepRenderer) {
    return applyTerminalResult(tile, status, pPrepRenderer, true);
}

} // namespace earth_engine
