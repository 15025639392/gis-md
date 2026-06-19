#include "TileTerminalLoadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

namespace {

void markUnknownTemporaryFailure(TilesetTile& tile) {
    tile.markContentFailedTemporarily();
}

void markUnknownPermanentFailure(TilesetTile& tile) {
    tile.markContentFailedPermanently();
}

} // namespace

TileTerminalLoadAction
TileTerminalLoadPolicy::applyTerrainTerminalResult(
    TilesetTile& tile,
    TerrainTileLoadStatus status) {
    TileTerminalLoadAction action;

    switch (status) {
        case TerrainTileLoadStatus::Empty: {
            action.markEmptyCacheKey = true;
            tile.markEmptyContentLoaded();

            const TilesetTile* ancestor = tile.parent;
            while (ancestor && ancestor->unconditionallyRefine) {
                ancestor = ancestor->parent;
            }
            const double parentError = ancestor
                ? ancestor->geometricError
                : tile.geometricError * 2.0;
            if (tile.geometricError >= parentError) {
                tile.unconditionallyRefine = true;
            }
            tile.markEmptyContentDone();
            action.resourcesDirty = true;
            break;
        }
        case TerrainTileLoadStatus::RetryLater:
        case TerrainTileLoadStatus::Cancelled:
            markUnknownTemporaryFailure(tile);
            action.resourcesDirty = true;
            break;
        case TerrainTileLoadStatus::Failed:
        case TerrainTileLoadStatus::Success:
            markUnknownPermanentFailure(tile);
            action.resourcesDirty = true;
            break;
    }

    return action;
}

TileTerminalLoadAction
TileTerminalLoadPolicy::applyContentTerminalResult(
    TilesetTile& tile,
    TileContentLoadStatus status) {
    TileTerminalLoadAction action;

    switch (status) {
        case TileContentLoadStatus::Empty:
            action.markEmptyCacheKey = true;
            tile.markEmptyContentDone();
            action.resourcesDirty = true;
            break;
        case TileContentLoadStatus::External:
            tile.markExternalContentDone();
            action.ensureChildren = true;
            action.resourcesDirty = true;
            break;
        case TileContentLoadStatus::RetryLater:
        case TileContentLoadStatus::Cancelled:
            markUnknownTemporaryFailure(tile);
            action.resourcesDirty = true;
            break;
        case TileContentLoadStatus::Failed:
        case TileContentLoadStatus::Render:
            markUnknownPermanentFailure(tile);
            action.resourcesDirty = true;
            break;
    }

    return action;
}

} // namespace earth_engine
