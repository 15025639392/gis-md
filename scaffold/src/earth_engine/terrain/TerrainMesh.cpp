#include "TerrainMesh.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>
#include <algorithm>

namespace earth_engine {

// ============================================================
// 辅助
// ============================================================

float TerrainMeshBuilder::sampleHeightFromTile(const TerrainTile* tile,
                                                 float texU, float texV) {
    if (!tile || !tile->valid()) return 0.0f;

    // texcoord: u=longitude 0→1 (west→east), v=latitude 0→1 (south→north)
    const auto& bounds = tile->bounds();
    double lngRad = bounds.west() + static_cast<double>(texU) * bounds.width();
    double latRad = bounds.south() + static_cast<double>(texV) * bounds.height();

    return tile->sampleHeight(lngRad, latRad);
}

// ============================================================
// 位移（无 skirt）
// ============================================================

GlobeMesh TerrainMeshBuilder::buildDisplaced(const GlobeMesh& baseMesh,
                                              const TerrainTile* terrainTile) {
    GlobeMesh result = baseMesh;

    constexpr double kEarthRadius = 6378137.0;

    for (auto& vertex : result.vertices) {
        float height = sampleHeightFromTile(terrainTile,
                                             vertex.texcoord[0],
                                             vertex.texcoord[1]);

        // 位移 = 法线方向 × 高度（单位球空间）
        double displacement = static_cast<double>(height) / kEarthRadius;

        vertex.position[0] += vertex.normal[0] * static_cast<float>(displacement);
        vertex.position[1] += vertex.normal[1] * static_cast<float>(displacement);
        vertex.position[2] += vertex.normal[2] * static_cast<float>(displacement);
    }

    return result;
}

// ============================================================
// 完整网格（含 skirt）
// ============================================================

GlobeMesh TerrainMeshBuilder::build(const GlobeMesh& baseMesh,
                                     const TerrainTile* terrainTile,
                                     float skirtHeightMeters) {
    // 基础位移
    GlobeMesh mesh = buildDisplaced(baseMesh, terrainTile);

    // Skirt：沿 tile 四边生成下沉三角形 curtain，防止相邻 tile 间裂缝。
    // 仅当有有效地形瓦片且 skirtHeight 为负值（向下）时生成。
    if (!terrainTile || !terrainTile->valid() || skirtHeightMeters >= 0.0f) {
        return mesh;
    }

    const auto& bounds = terrainTile->bounds();
    const auto& ellipsoid = Ellipsoid::WGS84();
    constexpr double kEarthRadius = 6378137.0;
    constexpr int kEdgeSegments = 24;  // 每边细分段数

    // 辅助：地理坐标 + 椭球高 → 单位球空间 GlobeVertex
    auto makeVertex = [&](double lngRad, double latRad, double hMeters) -> GlobeVertex {
        auto cart = Cartographic::fromRadians(lngRad, latRad, hMeters);
        Vec3 ecef = ellipsoid.cartographicToCartesian(cart);
        Vec3 n = ellipsoid.geodeticSurfaceNormal(cart);
        GlobeVertex v{};
        v.position[0] = static_cast<float>(ecef.x() / kEarthRadius);
        v.position[1] = static_cast<float>(ecef.y() / kEarthRadius);
        v.position[2] = static_cast<float>(ecef.z() / kEarthRadius);
        v.normal[0] = static_cast<float>(n.x());
        v.normal[1] = static_cast<float>(n.y());
        v.normal[2] = static_cast<float>(n.z());
        v.texcoord[0] = static_cast<float>((lngRad + glm::pi<double>()) /
                                             glm::two_pi<double>());
        v.texcoord[1] = static_cast<float>((latRad + glm::half_pi<double>()) /
                                             glm::pi<double>());
        return v;
    };

    // 沿一条边生成 skirt 条带（从上到下看：top 在边界，bottom 下沉）
    auto addSkirtEdge = [&](double lng0, double lat0,
                             double lng1, double lat1) {
        const uint32_t startIdx = static_cast<uint32_t>(mesh.vertices.size());

        for (int i = 0; i <= kEdgeSegments; ++i) {
            double t = static_cast<double>(i) / kEdgeSegments;
            double lng = lng0 + (lng1 - lng0) * t;
            double lat = lat0 + (lat1 - lat0) * t;
            float h = terrainTile->sampleHeight(lng, lat);

            // 顶部顶点（地形表面高度）
            mesh.vertices.push_back(
                makeVertex(lng, lat, static_cast<double>(h)));
            // 底部顶点（向下裙边）
            mesh.vertices.push_back(
                makeVertex(lng, lat, static_cast<double>(h) +
                           static_cast<double>(skirtHeightMeters)));
        }

        // 三角形条带：每段两个三角形（CCW 从外部看）
        for (int i = 0; i < kEdgeSegments; ++i) {
            uint32_t t0 = startIdx + static_cast<uint32_t>(i * 2);
            uint32_t b0 = t0 + 1;
            uint32_t t1 = startIdx + static_cast<uint32_t>((i + 1) * 2);
            uint32_t b1 = t1 + 1;
            mesh.indices.push_back(t0);
            mesh.indices.push_back(b0);
            mesh.indices.push_back(t1);
            mesh.indices.push_back(b0);
            mesh.indices.push_back(b1);
            mesh.indices.push_back(t1);
        }
    };

    // 四边（顺时针绕行，保证外表面法线朝外）
    double w = bounds.west(), s = bounds.south();
    double e = bounds.east(), n = bounds.north();

    addSkirtEdge(w, s, e, s);  // 南边（西→东）
    addSkirtEdge(e, s, e, n);  // 东边（南→北）
    addSkirtEdge(e, n, w, n);  // 北边（东→西）
    addSkirtEdge(w, n, w, s);  // 西边（北→南）

    return mesh;
}

} // namespace earth_engine
