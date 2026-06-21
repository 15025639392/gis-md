#pragma once

#include "RasterMappedToTilesetTile.h"
#include "SurfaceRasterBinding.h"
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
        if (!tile.content.renderContent.hasSurfaceMesh() || tile.children.size() >= 4) {
            return false;
        }

        const RasterOverlayDetails& details =
            tile.content.renderContent.rasterOverlayDetails();
        const Rectangle* subdivisionRectangle = nullptr;
        tile.rasterOverlayState.forEachMapping([&](const auto* mapped) {
            if (subdivisionRectangle ||
                !mapped ||
                !mapped->isMoreDetailAvailable()) {
                return;
            }
            const SurfaceRasterBinding binding =
                chooseSurfaceRasterBinding(mapped);
            if (binding.kind == SurfaceRasterBindingKind::None ||
                !binding.tile) {
                return;
            }
            subdivisionRectangle = details.findRectangleForOverlayProjection(
                binding.tile->getTileProvider().getProjection());
        });

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
