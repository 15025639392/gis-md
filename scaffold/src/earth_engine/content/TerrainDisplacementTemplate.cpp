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
    const Rectangle& tileBounds, int gridSize, bool generateIndices) {
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
    // 不生成索引时连 reserve 也免掉 —— dense 档这一下就是 1.57MB 的 malloc。
    if (generateIndices) {
        tmpl.indices.reserve(static_cast<size_t>(safeGrid) * safeGrid * 6);
    }

    // 规则经纬网格上 sin/cos(lng) 只随列变、sin/cos(lat) 只随行变 —— 外提成
    // O(n) 而非每顶点重算。dense 档(257²=66k 顶点)下原写法每顶点 ~4 次三角函数
    // = 26 万次,实测占 244ms(debug),是换档 336ms 卡顿的主项。
    // 另:原写法调 cartographicToCartesian 再单独调 geodeticSurfaceNormal,而前者
    // 内部本就算了一次法线(见 Ellipsoid.cpp:47)——等于每顶点白算一遍。这里改成
    // 算一次法线、复用给面点,算术与 Ellipsoid 内部逐步一致,结果不变。
    std::vector<double> cosLng(static_cast<size_t>(n));
    std::vector<double> sinLng(static_cast<size_t>(n));
    std::vector<double> uByColumn(static_cast<size_t>(n));
    for (int x = 0; x < n; ++x) {
        const double u = std::clamp(
            static_cast<double>(x) / static_cast<double>(safeGrid), 0.0, 1.0);
        const double lng = longitudeAt(tileBounds, u);
        uByColumn[static_cast<size_t>(x)] = u;
        cosLng[static_cast<size_t>(x)] = std::cos(lng);
        sinLng[static_cast<size_t>(x)] = std::sin(lng);
    }
    // radiiSquared_ 非公开;由公开的 radii() 逐分量平方得到(与 Ellipsoid 构造时
    // 的 radiiSquared_ 同值)。
    const Vec3& r = ellipsoid.radii();
    const Vec3 radiiSquared(r.x() * r.x(), r.y() * r.y(), r.z() * r.z());

    for (int y = 0; y < n; ++y) {
        const double v = std::clamp(
            static_cast<double>(y) / static_cast<double>(safeGrid), 0.0, 1.0);
        const double lat = mixValue(tileBounds.north(), tileBounds.south(), v);
        const double cosLat = std::cos(lat);
        const double sinLat = std::sin(lat);
        for (int x = 0; x < n; ++x) {
            const double u = uByColumn[static_cast<size_t>(x)];

            // = Ellipsoid::geodeticSurfaceNormal(Cartographic) 同式。
            const Vec3 normalEcef =
                Vec3(cosLat * cosLng[static_cast<size_t>(x)],
                     cosLat * sinLng[static_cast<size_t>(x)],
                     sinLat).normalized();
            // = Ellipsoid::cartographicToCartesian(height=0) 同式(复用上面的法线)。
            Vec3 k(radiiSquared.x() * normalEcef.x(),
                   radiiSquared.y() * normalEcef.y(),
                   radiiSquared.z() * normalEcef.z());
            const Vec3 surfaceEcef = k / std::sqrt(normalEcef.dot(k));

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
    for (int y = 0; generateIndices && y < safeGrid; ++y) {
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

    // 裙墙（P4，自适应版）：绕四条边挂一圈墙，遮住相邻 LOD/高度不连续处的接缝。
    // 关键——裙顶点**不预降**，而是与边顶点**逐字一致**（同 localPos/法线/UV），
    // 但打包时被标记为哨兵（heightDelta=-1）→ 位移 shader 认出后对裙顶点**跳过
    // 位移**（停在椭球面 h=0），而对应边顶点照常位移到真实地形高度。于是裙墙
    // 自动 = 从「位移后的边缘」垂到「椭球面」的那面墙：
    //   裙顶（边顶点）= 边缘位移点（h=真实高度）；裙底（裙顶点）= 边缘椭球点（h=0）。
    // 墙高 = 该边真实地形高度，逐瓦片自适应、精确覆盖「位移瓦片↔更低/未位移邻居」
    // 接缝、**零过冲**（绝不伸到椭球面以下）——根治旧版 skirtHeight=5*maxGeomError*
    // width 在粗 LOD 膨胀成 24-385 km 巨墙、被部分覆盖暴露成断崖/黑杠的问题。
    // localPos/法线/UV 逐列不变 → 仍可跨该 {LOD,row} 共享。
    tmpl.skirtVerticesBegin = static_cast<uint32_t>(tmpl.vertices.size());

    const auto gridIndex = [n](int x, int y) {
        return static_cast<uint32_t>(y * n + x);
    };
    // 边序 + 缠绕镜像 EllipsoidTerrainMeshBuilder::appendRegularGridSkirt
    // （west N→S, south W→E, east S→N, north E→W；(topA,topB,skirtA)/
    // (skirtA,topB,skirtB)），令墙面朝外不被背面剔除。
    const auto appendSkirtEdge = [&](const std::vector<uint32_t>& edge) {
        if (edge.size() < 2) {
            return;
        }
        const uint32_t firstSkirt =
            static_cast<uint32_t>(tmpl.vertices.size());
        for (uint32_t src : edge) {
            // 裙顶点 = 边顶点逐字复制（localPos/法线/UV 全同）。不改 localPos——
            // 墙由 shader 对裙顶点跳过位移自动撑开，无需在几何里预降。先取值拷贝
            // 再 push_back：vertices 只 reserve 了 n*n，追加裙顶点会扩容，直接传
            // vertices[src] 引用会在扩容后悬垂。
            const TerrainDisplacementTemplateVertex sv = tmpl.vertices[src];
            tmpl.vertices.push_back(sv);
        }
        // 裙**顶点**必须照常追加(顶点缓冲与 skirtVerticesBegin 都要它);
        // 只有索引可跳过(见 generateIndices 注释)。
        for (size_t i = 0; generateIndices && i + 1 < edge.size(); ++i) {
            const uint32_t topA = edge[i];
            const uint32_t topB = edge[i + 1];
            const uint32_t skirtA = firstSkirt + static_cast<uint32_t>(i);
            const uint32_t skirtB = firstSkirt + static_cast<uint32_t>(i + 1);
            tmpl.indices.push_back(topA);
            tmpl.indices.push_back(topB);
            tmpl.indices.push_back(skirtA);
            tmpl.indices.push_back(skirtA);
            tmpl.indices.push_back(topB);
            tmpl.indices.push_back(skirtB);
        }
    };

    std::vector<uint32_t> edge;
    edge.reserve(static_cast<size_t>(n));
    edge.clear();  // west: (0,0)→(0,n-1)  north→south
    for (int y = 0; y < n; ++y) edge.push_back(gridIndex(0, y));
    appendSkirtEdge(edge);
    edge.clear();  // south: (0,n-1)→(n-1,n-1)  west→east
    for (int x = 0; x < n; ++x) edge.push_back(gridIndex(x, n - 1));
    appendSkirtEdge(edge);
    edge.clear();  // east: (n-1,n-1)→(n-1,0)  south→north
    for (int y = n - 1; y >= 0; --y) edge.push_back(gridIndex(n - 1, y));
    appendSkirtEdge(edge);
    edge.clear();  // north: (n-1,0)→(0,0)  east→west
    for (int x = n - 1; x >= 0; --x) edge.push_back(gridIndex(x, 0));
    appendSkirtEdge(edge);

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
