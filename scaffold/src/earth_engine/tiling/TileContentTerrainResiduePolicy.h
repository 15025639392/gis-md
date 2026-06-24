#pragma once

#include "TilesetTile.h"

namespace earth_engine {

struct TileContentTerrainResiduePolicy {
    static bool hasAcceptedTerrainContent(const TilesetTile& tile) {
        return tile.content.renderContent.hasGltfContent() &&
               tile.content.renderContent.isTerrainRenderContent() &&
               tile.content.renderContent.hasRasterOverlayDetailsContent();
    }

    static bool hasRejectableResidue(const TilesetTile& tile) {
        if (hasAcceptedTerrainContent(tile)) {
            return false;
        }
        return tile.content.renderContent.hasRenderableTerrainContent() ||
               tile.content.renderContent.hasRetainedHeightmap() ||
               tile.content.renderContent.isRenderContentReady() ||
               tile.rasterOverlayState.mappingCount() > 0 ||
               tile.rasterOverlayState.hasMissingProjections();
    }

    static bool clearRejectableResidue(TilesetTile& tile) {
        if (!hasRejectableResidue(tile)) {
            return false;
        }
        tile.content.renderContent.clearRenderContent();
        tile.rasterOverlayState.releaseAndClearReferences(nullptr);
        return true;
    }
};

} // namespace earth_engine
