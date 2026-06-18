#pragma once

#include "RasterMappedToTilesetTile.h"
#include "TileChildMaterializer.h"
#include "TilesetTile.h"

#include <vector>

namespace earth_engine {

class TileRasterUpsampledChildMaterializer {
public:
    template <typename EnsureTileFn>
    static bool materialize(
        TilesetTile& tile,
        double defaultGeometricError,
        EnsureTileFn&& ensureTile) {
        if (!tile.mesh || tile.children.size() >= 4) {
            return false;
        }

        const RasterOverlayDetails& details =
            tile.mesh->rasterOverlayDetails;
        const Rectangle* subdivisionRectangle = nullptr;
        for (const auto& mapped : tile.rasterOverlays) {
            if (!mapped || !mapped->isMoreDetailAvailable()) {
                continue;
            }
            const RasterOverlayTile* readyTile = mapped->getReadyTile();
            if (!readyTile) {
                continue;
            }
            subdivisionRectangle = details.findRectangleForOverlayProjection(
                readyTile->getTileProvider().getProjection());
            if (subdivisionRectangle) {
                break;
            }
        }

        if (!subdivisionRectangle) {
            return false;
        }

        return TileChildMaterializer::materializeRasterUpsampledChildren(
            tile,
            *subdivisionRectangle,
            defaultGeometricError,
            ensureTile);
    }
};

} // namespace earth_engine
