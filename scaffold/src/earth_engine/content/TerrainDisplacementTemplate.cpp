#include "TerrainDisplacementTemplate.h"

#include <algorithm>
#include <cstddef>

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Transforms.h"
#include "../core/math/MathUtils.h"

namespace earth_engine {

namespace {

// 与 EllipsoidTerrainMeshBuilder 的采样约定逐字对齐（保证模板可等价替换）。
double mixValue(double a, double b, double t) { return a + (b - a) * t; }

double longitudeAt(const Rectangle& rectangle, double t) {
    double east = rectangle.east();
    if (rectangle.crossesAntimeridian()) {
        east += MathUtils::TwoPi;
    }
    double longitude = mixValue(rectangle.west(), east, t);
    if (longitude > MathUtils::OnePi) {
        longitude -= MathUtils::TwoPi;
    }
    return longitude;
}

Cartographic tileCenterCartographic(const Rectangle& bounds) {
    const double lat = mixValue(bounds.north(), bounds.south(), 0.5);
    const double lng = longitudeAt(bounds, 0.5);
    return Cartographic::fromRadians(lng, lat, 0.0);
}

void storeVec3(float (&dst)[3], const Vec3& v) {
    dst[0] = static_cast<float>(v.x());
    dst[1] = static_cast<float>(v.y());
    dst[2] = static_cast<float>(v.z());
}

}  // namespace

Mat4 terrainTemplateTileFrame(const Rectangle& tileBounds) {
    return Transforms::enuToEcef(tileCenterCartographic(tileBounds));
}

TerrainDisplacementTemplate buildTerrainDisplacementTemplate(
    const Rectangle& tileBounds, int gridSize) {
    const int safeGrid = std::max(1, gridSize);
    const int n = safeGrid + 1;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();

    // 瓦片中心 ENU 帧：把零高程面点/法线从 ECEF 变到局部帧。ecefToEnu 是
    // enuToEcef 的逆；对同 {LOD,row} 不同列，中心与顶点同步绕 Z 旋转，local 不变。
    const Cartographic center = tileCenterCartographic(tileBounds);
    const Mat4 ecefToEnu = Transforms::ecefToEnu(center);

    TerrainDisplacementTemplate tmpl;
    tmpl.gridSize = safeGrid;
    tmpl.vertices.reserve(static_cast<size_t>(n) * n);
    tmpl.indices.reserve(static_cast<size_t>(safeGrid) * safeGrid * 6);

    for (int y = 0; y < n; ++y) {
        const double v = std::clamp(
            static_cast<double>(y) / static_cast<double>(safeGrid), 0.0, 1.0);
        const double lat = mixValue(tileBounds.north(), tileBounds.south(), v);
        for (int x = 0; x < n; ++x) {
            const double u = std::clamp(
                static_cast<double>(x) / static_cast<double>(safeGrid), 0.0,
                1.0);
            const double lng = longitudeAt(tileBounds, u);

            const Cartographic surface = Cartographic::fromRadians(lng, lat, 0.0);
            const Vec3 surfaceEcef = ellipsoid.cartographicToCartesian(surface);
            const Vec3 normalEcef = ellipsoid.geodeticSurfaceNormal(surface);

            TerrainDisplacementTemplateVertex vertex;
            // 面点 = 点变换（含平移，原点=中心面点）；法线 = 方向变换（仅旋转）。
            storeVec3(vertex.localPos, ecefToEnu.transformPoint(surfaceEcef));
            storeVec3(vertex.localNormal, ecefToEnu.transformVector(normalEcef));
            vertex.uv[0] = static_cast<float>(u);
            vertex.uv[1] = static_cast<float>(v);
            tmpl.vertices.push_back(vertex);
        }
    }

    // 索引缠绕与 EllipsoidTerrainMeshBuilder 一致 (a,c,b,b,c,d)。
    for (int y = 0; y < safeGrid; ++y) {
        for (int x = 0; x < safeGrid; ++x) {
            const uint32_t a = static_cast<uint32_t>(y * n + x);
            const uint32_t b = static_cast<uint32_t>(y * n + x + 1);
            const uint32_t c = static_cast<uint32_t>((y + 1) * n + x);
            const uint32_t d = static_cast<uint32_t>((y + 1) * n + x + 1);
            tmpl.indices.push_back(a);
            tmpl.indices.push_back(c);
            tmpl.indices.push_back(b);
            tmpl.indices.push_back(b);
            tmpl.indices.push_back(c);
            tmpl.indices.push_back(d);
        }
    }

    return tmpl;
}

Vec3 reconstructTemplateWorldPosition(const TerrainDisplacementTemplate& tmpl,
                                      const Mat4& tileFrame, int vertexIndex,
                                      double height) {
    const TerrainDisplacementTemplateVertex& v = tmpl.vertices[static_cast<size_t>(
        vertexIndex)];
    const Vec3 localPos(v.localPos[0], v.localPos[1], v.localPos[2]);
    const Vec3 localNormal(v.localNormal[0], v.localNormal[1], v.localNormal[2]);
    const Vec3 displacedLocal = localPos + localNormal * height;
    return tileFrame.transformPoint(displacedLocal);
}

}  // namespace earth_engine
