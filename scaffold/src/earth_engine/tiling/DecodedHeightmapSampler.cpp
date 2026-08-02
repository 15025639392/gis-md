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
    double latitudeRadians,
    int renderGridSize) {
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

    // 渲染网格密度必须与该瓦片**本帧实际使用的位移模板档位**一致 —— 自适应
    // 密度后它不再恒为 64,由调用方从瓦片的常驻 draw 命令读出真值传入
    // (见 LoadedTerrainHeightSampler)。传 0 = 未知,退回 coarse 档。
    // 两侧不一致会让贴地矢量浮起或陷进地面。
    const int gridSize = renderGridSize > 0 ? renderGridSize
                                            : kTerrainDisplacementGridSize;
    const int cells = std::min(std::max(1, heightmap.tileSize - 1), gridSize);
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
