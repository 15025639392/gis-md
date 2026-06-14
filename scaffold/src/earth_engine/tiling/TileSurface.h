#pragma once

#include "SurfaceTile.h"
#include "TileKey.h"
#include "../core/math/Rectangle.h"
#include "../core/math/Vec3.h"

#include <array>

namespace earth_engine {

class TileScheme;
class TerrainTile;

/// CPU-side contract for draping a raster tile on the WGS84 ellipsoid.
/// Coordinates are radians, ECEF is meters, UV origin is texture top-left.
struct TileSurfaceVertex {
    Vec3 ecef;
    std::array<float, 2> uv;
};

struct TileTextureWindow {
    float offsetU = 0.0f;
    float offsetV = 0.0f;
    float scaleU = 1.0f;
    float scaleV = 1.0f;
};

class TileSurface {
public:
    static TileSurfaceVertex vertexForUnitUv(const Rectangle& tileBounds,
                                             double u,
                                             double v);

    // cesium-native: RasterOverlayUtilities::computeTranslationAndScale
    static TileTextureWindow computeTranslationAndScale(const Rectangle& geometryBounds,
                                                         const Rectangle& imageryBounds);

    static SurfaceTileMesh buildEllipsoidMesh(const Rectangle& tileBounds,
                                              int gridSize);

    /// 构建 ECEF SurfaceTile 地形网格。tileBounds 与 terrainTile 均为 WGS84
    /// cartographic bounds，height 单位 meter，输出 ECEF 单位 meter。
    /// WebMercator 有效纬度内按 Mercator-v 采样，OpenGlobus 极区 LonLat
    /// group 按 geographic-v 采样，避免极区被 Mercator clamp。
    static SurfaceTileMesh buildTerrainMesh(const Rectangle& tileBounds,
                                            const TerrainTile* terrainTile,
                                            int gridSize,
                                            double skirtHeightMeters = 0.0,
                                            const TerrainTile* parentTile = nullptr,
                                            bool useRawQuantizedMesh = true);

    /// 从 SurfaceTile mesh 顶点法线派生 OpenGlobus-style normal map。
    /// normal 以 ECEF/world space 单位向量编码到 RGBA8：rgb = normal * 0.5 + 0.5。
    /// 只使用规则 surface grid 顶点，terrain skirt 不进入 normal map。
    static SurfaceNormalMap buildNormalMap(const SurfaceTileMesh& mesh);

    static bool trianglesFaceOutward(const Rectangle& tileBounds);

    static Rectangle boundsForKey(const TileScheme& scheme, const TileKey& key);
};

} // namespace earth_engine
