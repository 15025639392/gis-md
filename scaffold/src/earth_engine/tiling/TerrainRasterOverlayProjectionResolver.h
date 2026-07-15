#pragma once

#include "SurfaceTile.h"
#include "TileKey.h"

#include <cstdint>
#include <limits>
#include <string>

namespace earth_engine {

struct TerrainRasterOverlayProjectionResolver {
    static RasterOverlayProjection forSchemeId(
        const std::string& schemeId) {
        return schemeId.find("WebMercator") != std::string::npos
            ? RasterOverlayProjection::WebMercator
            : RasterOverlayProjection::Geographic;
    }

    static RasterOverlayProjection forTileKey(const TileKey& key) {
        const std::string& schemeId = key.schemeId.str();
        if (schemeId != "OpenGlobus-Earth") {
            return forSchemeId(schemeId);
        }
        if (key.z < 0 ||
            key.z >= std::numeric_limits<uint32_t>::digits - 1) {
            return RasterOverlayProjection::Geographic;
        }
        const uint32_t tilesAtLevel = uint32_t{1} << key.z;
        return key.y >= 0 &&
                       static_cast<uint32_t>(key.y) < tilesAtLevel
            ? RasterOverlayProjection::WebMercator
            : RasterOverlayProjection::Geographic;
    }
};

} // namespace earth_engine
