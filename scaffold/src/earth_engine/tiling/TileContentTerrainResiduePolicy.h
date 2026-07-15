#pragma once

#include "TerrainRasterOverlayProjectionResolver.h"
#include "TilesetTile.h"

namespace earth_engine {

class IPrepareRendererResources;

struct TileContentTerrainResiduePolicy {
    static bool hasAcceptedTerrainContent(const TilesetTile& tile) {
        return tile.content.contentKind == TileContentKind::Render &&
               (tile.content.loadState == TileLoadState::ContentLoaded ||
                tile.content.loadState == TileLoadState::Done) &&
               tile.content.renderContent.hasGltfContent() &&
               tile.content.renderContent.isTerrainRenderContent();
    }

    static bool hasProtectedRetryableTerrainContent(const TilesetTile& tile) {
        return (tile.content.loadState == TileLoadState::ContentLoading ||
                tile.content.loadState == TileLoadState::FailedTemporarily) &&
               tile.content.renderContent.hasGltfContent() &&
               tile.content.renderContent.isTerrainRenderContent();
    }

    static bool hasProtectedRetryableFill(const TilesetTile& tile) {
        const TileLoadState state = tile.content.loadState;
        const TileRenderContentState& renderContent =
            tile.content.renderContent;
        const TileFillGeometrySignature* signature =
            renderContent.fillGeometrySignature();
        const bool retryable =
            state == TileLoadState::Unloaded ||
            state == TileLoadState::ContentLoading ||
            state == TileLoadState::FailedTemporarily;
        return retryable &&
               tile.content.contentKind == TileContentKind::Unknown &&
               renderContent.isFillReady() &&
               signature &&
               signature->bounds == tile.bounds &&
               signature->projection ==
                   TerrainRasterOverlayProjectionResolver::forTileKey(
                       tile.key) &&
               !renderContent.hasGltfContent() &&
               !renderContent.hasRetainedHeightmap() &&
               !renderContent.isRenderContentReady() &&
               !tile.rasterOverlayState.hasMissingProjections();
    }

    static bool hasRejectableResidue(const TilesetTile& tile) {
        if (hasAcceptedTerrainContent(tile) ||
            hasProtectedRetryableTerrainContent(tile) ||
            hasProtectedRetryableFill(tile)) {
            return false;
        }
        return tile.content.renderContent.hasRenderableTerrainContent() ||
               tile.content.renderContent.hasRetainedHeightmap() ||
               tile.content.renderContent.isRenderContentReady() ||
               tile.content.renderContent.hasFillModel() ||
               tile.content.renderContent.hasFillResources() ||
               tile.rasterOverlayState.mappingCount() > 0 ||
               tile.rasterOverlayState.hasMissingProjections();
    }

    static bool clearRejectableResidue(
        TilesetTile& tile,
        IPrepareRendererResources* pPrepRenderer = nullptr) {
        if (!hasRejectableResidue(tile)) {
            return false;
        }
        tile.content.renderContent.clearRenderContent();
        tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
        tile.notifyChildMaterializationStateChanged();
        return true;
    }
};

} // namespace earth_engine
