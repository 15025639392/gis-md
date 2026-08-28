#include "RasterBindingSet.h"

#include "TilesetTile.h"
#include "../layers/ActivatedRasterOverlay.h"

namespace earth_engine {

RasterBindingSet RasterBindingSet::resolve(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& overlays) {
    RasterBindingSet result;
    result.bindings_.reserve(overlays.size());
    for (size_t i = 0; i < overlays.size(); ++i) {
        ActivatedRasterOverlay* overlay = overlays[i];
        const RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        SurfaceRasterBinding surface = chooseSurfaceRasterBinding(mapped);
        const bool allowedByPolicy = rasterOverlayBindingAllowedByPolicy(
            overlay,
            mapped,
            surface);
        const int32_t textureCoordinateId =
            mapped ? mapped->getTextureCoordinateID() : -1;
        result.bindings_.push_back(RasterBinding{
            i,
            overlay,
            mapped,
            std::move(surface),
            textureCoordinateId,
            overlay ? overlay->opacity() : 1.0f,
            allowedByPolicy});
    }
    return result;
}

} // namespace earth_engine
