#pragma once

#include "RasterMappedToTilesetTile.h"
#include "TileChildMaterializer.h"
#include "TilesetTile.h"

#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <vector>

namespace earth_engine {

class TileRasterUpsampledChildMaterializer {
public:
    template <typename EnsureTileFn>
    static bool materialize(
        TilesetTile& tile,
        double defaultGeometricError,
        EnsureTileFn&& ensureTile) {
        if (!tile.content.renderContent.hasSurfaceMesh() || tile.children.size() >= 4) {
            return false;
        }

        const RasterOverlayDetails& details =
            tile.content.renderContent.rasterOverlayDetails();
        Rectangle subdivisionRectangle = tile.bounds;
        if (details.boundingRegion.minimumHeight <=
                details.boundingRegion.maximumHeight &&
            !details.boundingRegion.rectangle.isEmpty()) {
            subdivisionRectangle = details.boundingRegion.rectangle;
        }
        bool hasMoreRasterDetail = false;
        tile.rasterOverlayState.forEachMapping([&](const auto* mapped) {
            if (hasMoreRasterDetail ||
                !mapped ||
                !mapped->isMoreDetailAvailable()) {
                return;
            }
            const RasterOverlayTile* readyTile = mapped->getReadyTile();
            if (!readyTile) {
                return;
            }
            if (details.findRectangleForOverlayProjection(
                    readyTile->getTileProvider().getProjection())) {
                hasMoreRasterDetail = true;
            }
        });

        if (!hasMoreRasterDetail) {
            return false;
        }

        return TileChildMaterializer::materializeRasterUpsampledChildren(
            tile,
            subdivisionRectangle,
            defaultGeometricError,
            ensureTile);
    }
};

} // namespace earth_engine
