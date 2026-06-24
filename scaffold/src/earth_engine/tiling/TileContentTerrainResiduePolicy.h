#pragma once

#include "TilesetTile.h"

namespace earth_engine {

class IPrepareRendererResources;

struct TileContentTerrainResiduePolicy {
    static bool hasAcceptedTerrainContent(const TilesetTile& tile) {
        return tile.content.contentKind == TileContentKind::Render &&
               (tile.content.loadState == TileLoadState::ContentLoaded ||
                tile.content.loadState == TileLoadState::Done) &&
               tile.content.renderContent.hasGltfContent() &&
               tile.content.renderContent.isTerrainRenderContent() &&
               tile.content.renderContent.hasRasterOverlayDetailsContent();
    }

    static bool hasProtectedRetryableTerrainContent(const TilesetTile& tile) {
        return (tile.content.loadState == TileLoadState::ContentLoading ||
                tile.content.loadState == TileLoadState::FailedTemporarily) &&
               tile.content.renderContent.hasGltfContent() &&
               tile.content.renderContent.isTerrainRenderContent();
    }

    static bool hasRejectableResidue(const TilesetTile& tile) {
        if (hasAcceptedTerrainContent(tile) ||
            hasProtectedRetryableTerrainContent(tile)) {
            return false;
        }
        return tile.content.renderContent.hasRenderableTerrainContent() ||
               tile.content.renderContent.hasRetainedHeightmap() ||
               tile.content.renderContent.isRenderContentReady() ||
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
        return true;
    }
};

} // namespace earth_engine
