#include "DecodedHeightmapSampler.h"

#include "TerrainDisplacementTemplatePool.h"
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

float DecodedHeightmapSampler::sampleHeightRenderGrid(
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

    // 渲染网格密度与 HeightmapTerrainContentProvider / 位移模板一致:
    // min(tileSize-1, kTerrainDisplacementGridSize)。
    const int cells = std::min(std::max(1, heightmap.tileSize - 1),
                               kTerrainDisplacementGridSize);
    const double gx = u * cells;
    const double gy = v * cells;
    int i0 = std::min(static_cast<int>(gx), cells - 1);
    int j0 = std::min(static_cast<int>(gy), cells - 1);
    const double fx = gx - i0;
    const double fy = gy - j0;

    const auto nodeHeight = [&](int i, int j) -> float {
        const float h = heightmap.sampleBilinear(
            static_cast<float>(static_cast<double>(i) / cells),
            static_cast<float>(static_cast<double>(j) / cells));
        return heightmap.isNoData(h) ? 0.0f : h;
    };
    const double h00 = nodeHeight(i0, j0);
    const double h10 = nodeHeight(i0 + 1, j0);
    const double h01 = nodeHeight(i0, j0 + 1);
    const double h11 = nodeHeight(i0 + 1, j0 + 1);
    return static_cast<float>(
        (h00 * (1.0 - fx) + h10 * fx) * (1.0 - fy) +
        (h01 * (1.0 - fx) + h11 * fx) * fy);
}

} // namespace earth_engine
