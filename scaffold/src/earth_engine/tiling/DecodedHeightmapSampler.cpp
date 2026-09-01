#include "DecodedHeightmapSampler.h"

#include "TerrainDisplacementTemplatePool.h"
#include "../core/math/Rectangle.h"
#include "../providers/TerrainProvider.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace earth_engine {

namespace {

float interpolateRenderCell(double h00, double h10,
                            double h01, double h11,
                            double fx, double fy) {
    // TerrainDisplacementTemplate emits the fixed diagonal a-c-b / b-c-d.
    // Interpolate the actual two GPU triangles, not the four-node bilinear
    // patch (which is a different curved surface whenever the cross term is
    // non-zero).
    if (fx + fy <= 1.0) {
        return static_cast<float>(
            h00 + (h10 - h00) * fx + (h01 - h00) * fy);
    }
    return static_cast<float>(
        h10 * (1.0 - fy) + h01 * (1.0 - fx) +
        h11 * (fx + fy - 1.0));
}

template <typename NodeHeight>
float sampleMorphedRenderGrid(
    double u, double v, int cells, float morph,
    const NodeHeight& nodeHeight) {
    // The baked mesh builder deliberately omits geomorph deltas for odd
    // grids. Displacement production grids are 64/256 (even). Keep odd-grid
    // callers on their emitted fine surface instead of inventing a coarse
    // contract no renderer consumes.
    if ((cells & 1) != 0) morph = 1.0f;
    const double gx = u * cells;
    const double gy = v * cells;
    const int i0 = std::min(static_cast<int>(gx), cells - 1);
    const int j0 = std::min(static_cast<int>(gy), cells - 1);
    const double fx = gx - i0;
    const double fy = gy - j0;
    struct CachedNode { int i = -1, j = -1; double height = 0.0; };
    std::array<CachedNode, 12> cache{};
    std::size_t cacheSize = 0;
    const auto cachedNodeHeight = [&](int i, int j) -> double {
        for (std::size_t k = 0; k < cacheSize; ++k) {
            if (cache[k].i == i && cache[k].j == j) return cache[k].height;
        }
        const double height = nodeHeight(i, j);
        if (cacheSize < cache.size()) cache[cacheSize++] = {i, j, height};
        return height;
    };
    const auto effectiveNode = [&](int i, int j) {
        const int coarseI0 = (i / 2) * 2;
        const int coarseJ0 = (j / 2) * 2;
        const int coarseI1 = std::min(coarseI0 + 2, cells);
        const int coarseJ1 = std::min(coarseJ0 + 2, cells);
        const double coarseFx = static_cast<double>(i - coarseI0) * 0.5;
        const double coarseFy = static_cast<double>(j - coarseJ0) * 0.5;
        const double coarseTop =
            cachedNodeHeight(coarseI0, coarseJ0) * (1.0 - coarseFx) +
            cachedNodeHeight(coarseI1, coarseJ0) * coarseFx;
        const double coarseBottom =
            cachedNodeHeight(coarseI0, coarseJ1) * (1.0 - coarseFx) +
            cachedNodeHeight(coarseI1, coarseJ1) * coarseFx;
        const double coarse =
            coarseTop * (1.0 - coarseFy) + coarseBottom * coarseFy;
        const double fine = cachedNodeHeight(i, j);
        return coarse + (fine - coarse) * morph;
    };
    return interpolateRenderCell(
        effectiveNode(i0, j0), effectiveNode(i0 + 1, j0),
        effectiveNode(i0, j0 + 1), effectiveNode(i0 + 1, j0 + 1),
        fx, fy);
}

} // namespace

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
    return interpolateRenderCell(h00, h10, h01, h11, fx, fy);
}

float DecodedHeightmapSampler::sampleHeightRenderGridQuantized(
    const DecodedHeightmap& heightmap,
    const Rectangle& sourceBounds,
    double longitudeRadians,
    double latitudeRadians,
    int renderGridSize,
    float minHeight,
    float heightRange) {
    return sampleHeightRenderGridQuantizedDecoded(
        heightmap, sourceBounds, longitudeRadians, latitudeRadians,
        renderGridSize, minHeight, heightRange, minHeight, heightRange);
}

float DecodedHeightmapSampler::sampleHeightRenderGridQuantizedDecoded(
    const DecodedHeightmap& heightmap,
    const Rectangle& sourceBounds,
    double longitudeRadians,
    double latitudeRadians,
    int renderGridSize,
    float encodeMinHeight,
    float encodeHeightRange,
    float decodeMinHeight,
    float decodeHeightRange) {
    if (!heightmap.valid()) return 0.0f;
    double u = (longitudeRadians - sourceBounds.west()) / sourceBounds.width();
    double v = (sourceBounds.north() - latitudeRadians) / sourceBounds.height();
    if (u < -1e-12 || u > 1.0 + 1e-12 ||
        v < -1e-12 || v > 1.0 + 1e-12) {
        return 0.0f;
    }
    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
    const int gridSize = renderGridSize > 0 ? renderGridSize
                                            : kTerrainDisplacementGridSize;
    const int cells = std::min(std::max(1, heightmap.tileSize - 1), gridSize);
    const double gx = u * cells;
    const double gy = v * cells;
    const int i0 = std::min(static_cast<int>(gx), cells - 1);
    const int j0 = std::min(static_cast<int>(gy), cells - 1);
    const double fx = gx - i0;
    const double fy = gy - j0;
    const float encodeRange = std::max(1e-3f, encodeHeightRange);
    const auto nodeHeight = [&](int i, int j) {
        float height = heightmap.sampleBilinear(
            static_cast<float>(static_cast<double>(i) / cells),
            static_cast<float>(static_cast<double>(j) / cells));
        if (heightmap.isNoData(height)) height = 0.0f;
        const float normalized =
            std::clamp((height - encodeMinHeight) / encodeRange, 0.0f, 1.0f);
        const auto code = static_cast<std::uint32_t>(
            std::lround(normalized * 65535.0f));
        return decodeMinHeight +
               (static_cast<float>(code) / 65535.0f) * decodeHeightRange;
    };
    const double h00 = nodeHeight(i0, j0);
    const double h10 = nodeHeight(i0 + 1, j0);
    const double h01 = nodeHeight(i0, j0 + 1);
    const double h11 = nodeHeight(i0 + 1, j0 + 1);
    return interpolateRenderCell(h00, h10, h01, h11, fx, fy);
}

float DecodedHeightmapSampler::sampleHeightRenderGridMorphed(
    const DecodedHeightmap& heightmap,
    const Rectangle& sourceBounds,
    double longitudeRadians,
    double latitudeRadians,
    int renderGridSize,
    float morph) {
    if (!heightmap.valid()) return 0.0f;
    double u = (longitudeRadians - sourceBounds.west()) / sourceBounds.width();
    double v = (sourceBounds.north() - latitudeRadians) / sourceBounds.height();
    if (u < -1e-12 || u > 1.0 + 1e-12 ||
        v < -1e-12 || v > 1.0 + 1e-12) return 0.0f;
    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
    const int gridSize = renderGridSize > 0 ? renderGridSize
                                            : kTerrainDisplacementGridSize;
    const int cells = std::min(std::max(1, heightmap.tileSize - 1), gridSize);
    const auto nodeHeight = [&](int i, int j) -> float {
        const float h = heightmap.sampleBilinear(
            static_cast<float>(static_cast<double>(i) / cells),
            static_cast<float>(static_cast<double>(j) / cells));
        return heightmap.isNoData(h) ? 0.0f : h;
    };
    return sampleMorphedRenderGrid(u, v, cells,
                                   std::clamp(morph, 0.0f, 1.0f),
                                   nodeHeight);
}

float DecodedHeightmapSampler::sampleHeightRenderGridMorphedWebMercatorV(
    const DecodedHeightmap& heightmap,
    const Rectangle& sourceBounds,
    double longitudeRadians,
    double latitudeRadians,
    int renderGridSize,
    float morph) {
    if (!heightmap.valid()) return 0.0f;
    double u = (longitudeRadians - sourceBounds.west()) / sourceBounds.width();
    double v = (sourceBounds.north() - latitudeRadians) / sourceBounds.height();
    if (u < -1e-12 || u > 1.0 + 1e-12 ||
        v < -1e-12 || v > 1.0 + 1e-12) return 0.0f;
    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
    const int gridSize = renderGridSize > 0 ? renderGridSize
                                            : kTerrainDisplacementGridSize;
    const int cells = std::min(std::max(1, heightmap.tileSize - 1), gridSize);
    const auto mercatorY = [](double latitude) {
        constexpr double kLimit = 1.4844222297453324;
        latitude = std::clamp(latitude, -kLimit, kLimit);
        return std::log(std::tan(0.7853981633974483 + latitude * 0.5));
    };
    const double northY = mercatorY(sourceBounds.north());
    const double southY = mercatorY(sourceBounds.south());
    const double yRange = northY - southY;
    if (!(yRange > 0.0)) return 0.0f;
    const auto nodeHeight = [&](int i, int j) -> float {
        const double nodeU = static_cast<double>(i) / cells;
        const double linearV = static_cast<double>(j) / cells;
        const double latitude = sourceBounds.north() -
                                linearV * sourceBounds.height();
        const double projectedV = std::clamp(
            (northY - mercatorY(latitude)) / yRange, 0.0, 1.0);
        const float h = heightmap.sampleBilinear(
            static_cast<float>(nodeU), static_cast<float>(projectedV));
        return heightmap.isNoData(h) ? 0.0f : h;
    };
    return sampleMorphedRenderGrid(u, v, cells,
                                   std::clamp(morph, 0.0f, 1.0f),
                                   nodeHeight);
}

float DecodedHeightmapSampler::sampleHeightRenderGridQuantizedDecodedMorphed(
    const DecodedHeightmap& heightmap,
    const Rectangle& sourceBounds,
    double longitudeRadians,
    double latitudeRadians,
    int renderGridSize,
    float morph,
    float encodeMinHeight,
    float encodeHeightRange,
    float decodeMinHeight,
    float decodeHeightRange) {
    if (!heightmap.valid()) return 0.0f;
    double u = (longitudeRadians - sourceBounds.west()) / sourceBounds.width();
    double v = (sourceBounds.north() - latitudeRadians) / sourceBounds.height();
    if (u < -1e-12 || u > 1.0 + 1e-12 ||
        v < -1e-12 || v > 1.0 + 1e-12) return 0.0f;
    u = std::clamp(u, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);
    const int gridSize = renderGridSize > 0 ? renderGridSize
                                            : kTerrainDisplacementGridSize;
    const int cells = std::min(std::max(1, heightmap.tileSize - 1), gridSize);
    const float encodeRange = std::max(1e-3f, encodeHeightRange);
    const auto nodeHeight = [&](int i, int j) {
        float height = heightmap.sampleBilinear(
            static_cast<float>(static_cast<double>(i) / cells),
            static_cast<float>(static_cast<double>(j) / cells));
        if (heightmap.isNoData(height)) height = 0.0f;
        const float normalized = std::clamp(
            (height - encodeMinHeight) / encodeRange, 0.0f, 1.0f);
        const auto code = static_cast<std::uint32_t>(
            std::lround(normalized * 65535.0f));
        return decodeMinHeight +
               (static_cast<float>(code) / 65535.0f) * decodeHeightRange;
    };
    return sampleMorphedRenderGrid(u, v, cells,
                                   std::clamp(morph, 0.0f, 1.0f),
                                   nodeHeight);
}

} // namespace earth_engine
