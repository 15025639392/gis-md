#include "DecodedHeightmapSampler.h"

#include "../core/math/Rectangle.h"
#include "../providers/TerrainProvider.h"

#include <algorithm>

namespace earth_engine {

float DecodedHeightmapSampler::sampleHeight(
    const DecodedHeightmap& heightmap,
    const Rectangle& sourceBounds,
    double longitudeRadians,
    double latitudeRadians) {
    if (!heightmap.valid()) return 0.0f;

    double u = (longitudeRadians - sourceBounds.west()) / sourceBounds.width();
    double v = (sourceBounds.north() - latitudeRadians) / sourceBounds.height();
    constexpr double kTileCoordinateEpsilon = 1e-12;
    const auto clampTileCoordinate = [](double& coordinate) {
        if (coordinate < -kTileCoordinateEpsilon ||
            coordinate > 1.0 + kTileCoordinateEpsilon) {
            return false;
        }
        coordinate = std::clamp(coordinate, 0.0, 1.0);
        return true;
    };
    if (!clampTileCoordinate(u) || !clampTileCoordinate(v)) {
        return 0.0f;
    }

    const float h = heightmap.sampleBilinear(
        static_cast<float>(u),
        static_cast<float>(v));
    if (heightmap.isNoData(h)) return 0.0f;
    return h;
}

} // namespace earth_engine
