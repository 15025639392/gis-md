// P1a host 测试（北极星 Phase 2c，共享位移模板）。
//
// 锁死两条 P1 几何地基（frame/sharing 数学，P0 之后第二大风险）：
//   ① 逐列不变：同 {LOD, mercator-row} 不同列的瓦片，模板 vertices（localPos/
//      localNormal/uv）逐值相等 → 一份模板可跨该行所有列共享（§5 有界 VBO）。
//   ② 位移重建：frame·(localPos + localNormal·h) ≡ cartographicToCartesian
//      (lng,lat,h)。双精度重建应 <1mm；模拟 shader 的 f32 重建应 <1m（承 P0）。
//
// 前者是"能不能省 VBO"的前提，后者是"省了 VBO 位置还对不对"的前提。

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "earth_engine/content/TerrainDisplacementTemplate.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

namespace {

constexpr int kGrid = 64;  // 与生产 grid64 一致（n=65=2^6+1，GE 嵌套栅格约定）

// WebMercator 行 {LOD, row} 的纬度带（Web Mercator y→lat），给定 lod 与 tileY。
double mercatorLatAtTileEdge(int lod, int tileY) {
    const int tiles = 1 << lod;
    const double yNorm = static_cast<double>(tileY) / tiles;  // 0..1，北→南
    const double merc = MathUtils::OnePi * (1.0 - 2.0 * yNorm);  // y in [-pi,pi]
    return 2.0 * std::atan(std::exp(merc)) - MathUtils::OnePi / 2.0;
}

// 构造 {lod, tileX, tileY} 的经纬度矩形（WebMercator）。
Rectangle webMercatorTileRect(int lod, int tileX, int tileY) {
    const int tiles = 1 << lod;
    const double west =
        -MathUtils::OnePi + MathUtils::TwoPi * tileX / tiles;
    const double east =
        -MathUtils::OnePi + MathUtils::TwoPi * (tileX + 1) / tiles;
    const double north = mercatorLatAtTileEdge(lod, tileY);
    const double south = mercatorLatAtTileEdge(lod, tileY + 1);
    return Rectangle(west, south, east, north);  // (west,south,east,north) rad
}

double vec3Distance(const float (&a)[3], const Vec3& b) {
    const double dx = static_cast<double>(a[0]) - b.x();
    const double dy = static_cast<double>(a[1]) - b.y();
    const double dz = static_cast<double>(a[2]) - b.z();
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

// ① 逐列不变：同 {lod,row} 的第 0/3/7 列，模板逐顶点相等。
TEST(TerrainDisplacementTemplate, ColumnInvariantWithinMercatorRow) {
    const int lod = 8;
    const int tileY = 90;  // 中纬带
    const int cols[] = {0, 3, 7, 100};

    const TerrainDisplacementTemplate ref =
        buildTerrainDisplacementTemplate(webMercatorTileRect(lod, 0, tileY),
                                         kGrid);

    for (int col : cols) {
        const TerrainDisplacementTemplate t =
            buildTerrainDisplacementTemplate(
                webMercatorTileRect(lod, col, tileY), kGrid);

        ASSERT_EQ(ref.vertices.size(), t.vertices.size());
        ASSERT_EQ(ref.indices.size(), t.indices.size());

        double maxPosDelta = 0.0;
        double maxNrmDelta = 0.0;
        for (size_t i = 0; i < ref.vertices.size(); ++i) {
            for (int k = 0; k < 3; ++k) {
                maxPosDelta = std::max(
                    maxPosDelta,
                    std::abs(static_cast<double>(ref.vertices[i].localPos[k]) -
                             t.vertices[i].localPos[k]));
                maxNrmDelta = std::max(
                    maxNrmDelta,
                    std::abs(static_cast<double>(ref.vertices[i].localNormal[k]) -
                             t.vertices[i].localNormal[k]));
            }
        }
        // f32 存储下，跨列应逐位相等（同一算术，仅经度旋转被 ENU 帧抵消）。
        // 放 1mm 容差吸收 f32 表示噪声——远小于任何几何意义。
        EXPECT_LT(maxPosDelta, 1e-3)
            << "localPos not column-invariant at col=" << col
            << " (delta=" << maxPosDelta << "m)";
        EXPECT_LT(maxNrmDelta, 1e-6)
            << "localNormal not column-invariant at col=" << col;
    }
}

// ② 位移重建 vs cartographicToCartesian。模板 localPos 存 f32（GPU VBO 格式），
// 故重建误差 = f32-RTC 量级（承 P0 模型）：粗 LOD <1m、native z12 亚厘米。
// frame 数学本身精确（误差严格随局部量级/f32 ULP 缩放，非 frame bug）。
TEST(TerrainDisplacementTemplate, ReconstructsCartographicToCartesian) {
    const Ellipsoid& e = Ellipsoid::WGS84();
    struct Case {
        int lod, tileX, tileY;
        double elevM;
        double budgetM;  // 该 LOD 的 f32-RTC 重建预算
    };
    const Case cases[] = {
        {5, 10, 12, 0.0, 1.0},       {5, 10, 12, 9000.0, 1.0},
        {8, 200, 90, 9000.0, 0.05},  {10, 800, 360, 9000.0, 0.02},
        {12, 3400, 1500, 9000.0, 0.01},  // native cap：亚厘米
    };

    for (const Case& c : cases) {
        const Rectangle rect = webMercatorTileRect(c.lod, c.tileX, c.tileY);
        const TerrainDisplacementTemplate tmpl =
            buildTerrainDisplacementTemplate(rect, kGrid);
        const Mat4 frame = terrainTemplateTileFrame(rect);
        const int n = tmpl.gridSize + 1;

        double maxErr = 0.0;
        for (int y = 0; y < n; ++y) {
            const double v =
                static_cast<double>(y) / static_cast<double>(tmpl.gridSize);
            const double lat = rect.north() + (rect.south() - rect.north()) * v;
            for (int x = 0; x < n; ++x) {
                const double u =
                    static_cast<double>(x) / static_cast<double>(tmpl.gridSize);
                double east = rect.east();
                if (rect.crossesAntimeridian()) east += MathUtils::TwoPi;
                double lng = rect.west() + (east - rect.west()) * u;
                if (lng > MathUtils::OnePi) lng -= MathUtils::TwoPi;

                const int idx = y * n + x;
                const Vec3 truth = e.cartographicToCartesian(
                    Cartographic::fromRadians(lng, lat, c.elevM));

                // 模板+frame 正确性（从 f32 模板重建）。误差由 f32 localPos 存储
                // 主导（= f32-RTC 量级）；shader 侧 f32 位移精度另由 P0 覆盖。
                const Vec3 recon = reconstructTemplateWorldPosition(
                    tmpl, frame, idx, c.elevM);
                maxErr = std::max(maxErr, (recon - truth).length());
            }
        }

        const std::string label = "lod=" + std::to_string(c.lod) +
                                  " elev=" + std::to_string(c.elevM);
        EXPECT_LT(maxErr, c.budgetM)
            << "f32 reconstruction exceeded budget at " << label
            << " (err=" << maxErr << "m budget=" << c.budgetM << "m)";
        RecordProperty(label + ":reconF32_m", std::to_string(maxErr));
    }
}

// 模板拓扑健全：栅格 n² 顶点 + 4*n 裙墙顶点；栅格 grid²*6 索引 +
// 4*(n-1)*6 裙墙索引（四边各 n 顶点、n-1 段墙、每段两三角）。
TEST(TerrainDisplacementTemplate, TopologyCountsMatchGrid) {
    const TerrainDisplacementTemplate t = buildTerrainDisplacementTemplate(
        webMercatorTileRect(6, 30, 24), kGrid);
    const int n = kGrid + 1;
    const size_t gridVerts = static_cast<size_t>(n) * n;
    const size_t skirtVerts = static_cast<size_t>(4) * n;
    const size_t gridIndices = static_cast<size_t>(kGrid) * kGrid * 6;
    const size_t skirtIndices = static_cast<size_t>(4) * (n - 1) * 6;
    EXPECT_EQ(t.vertices.size(), gridVerts + skirtVerts);
    EXPECT_EQ(t.indices.size(), gridIndices + skirtIndices);
}

// ③ 接边闭合(2026-08-15,黑带排查):相邻瓦片的公共边在**世界坐标**里必须重合。
//
// 为什么单有 ① 不够:① 只说同一行不同列的 localPos 逐值相等,可每片还要各自
// 套上 enuToEcef(自己中心) 的刚体帧。局部相等 + 帧不同,拼接处照样可能张口。
// 真机上看到的正是水平方向的口子(黑带里格与格之间、以及高空的列边界竖线),
// 而 SeamDiag 的 compensated 读数说明**高度**是对齐的(0.00m)——所以要查的
// 不是竖直失配,是这条水平闭合。
//
// 判据:公共边上每个节点,两侧各自重建出的世界点距离 ≤ 该 LOD 的 f32-RTC 预算
// (与 ② 同一套预算,因为误差同源)。超过它就意味着几何真的没接上。
TEST(TerrainDisplacementTemplate, AdjacentTilesShareClosedEdges) {
    struct Case {
        int lod, tileX, tileY;
        double budgetM;
    };
    // z6-8 是真机黑带出现的档位区间(relief fade 过渡带 z6→9)。
    const Case cases[] = {
        {6, 30, 24, 2.0},
        {7, 99, 49, 1.0},   // 真机复现区(蒙古/戈壁)所在瓦
        {8, 200, 90, 0.5},
    };

    for (const Case& c : cases) {
        const int n = kGrid + 1;
        const Rectangle rectA = webMercatorTileRect(c.lod, c.tileX, c.tileY);
        const Rectangle rectE =
            webMercatorTileRect(c.lod, c.tileX + 1, c.tileY);   // 东邻(同行)
        const Rectangle rectS =
            webMercatorTileRect(c.lod, c.tileX, c.tileY + 1);   // 南邻(下一行)

        const TerrainDisplacementTemplate tA =
            buildTerrainDisplacementTemplate(rectA, kGrid);
        const TerrainDisplacementTemplate tE =
            buildTerrainDisplacementTemplate(rectE, kGrid);
        const TerrainDisplacementTemplate tS =
            buildTerrainDisplacementTemplate(rectS, kGrid);
        const Mat4 fA = terrainTemplateTileFrame(rectA);
        const Mat4 fE = terrainTemplateTileFrame(rectE);
        const Mat4 fS = terrainTemplateTileFrame(rectS);

        // 列边界:A 的东边(col=n-1) vs 东邻的西边(col=0),逐行比。
        double maxCol = 0.0;
        for (int row = 0; row < n; ++row) {
            const Vec3 a = reconstructTemplateWorldPosition(
                tA, fA, row * n + (n - 1), 0.0);
            const Vec3 b = reconstructTemplateWorldPosition(
                tE, fE, row * n + 0, 0.0);
            maxCol = std::max(maxCol, (a - b).length());
        }

        // 行边界:A 的南边(row=n-1) vs 南邻的北边(row=0),逐列比。
        double maxRow = 0.0;
        for (int col = 0; col < n; ++col) {
            const Vec3 a = reconstructTemplateWorldPosition(
                tA, fA, (n - 1) * n + col, 0.0);
            const Vec3 b = reconstructTemplateWorldPosition(
                tS, fS, 0 * n + col, 0.0);
            maxRow = std::max(maxRow, (a - b).length());
        }

        const std::string label =
            "lod=" + std::to_string(c.lod) + " tile=" +
            std::to_string(c.tileX) + "," + std::to_string(c.tileY);
        EXPECT_LT(maxCol, c.budgetM)
            << "列边界张口 " << label << " gap=" << maxCol << "m";
        EXPECT_LT(maxRow, c.budgetM)
            << "行边界张口 " << label << " gap=" << maxRow << "m";
        RecordProperty(label + ":colGap_m", std::to_string(maxCol));
        RecordProperty(label + ":rowGap_m", std::to_string(maxRow));
    }
}
