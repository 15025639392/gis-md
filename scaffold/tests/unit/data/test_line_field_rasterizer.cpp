#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "earth_engine/data/LineFieldRasterizer.h"
#include "earth_engine/data/StyleFilter.h"

using namespace earth_engine;

namespace {

constexpr uint32_t kExtent = 4096;

MvtPoint pt(int x, int y) { return MvtPoint{x, y}; }

MvtFeature lineFeature(std::vector<std::vector<MvtPoint>> paths) {
    MvtFeature f;
    f.type = MvtGeomType::LineString;
    f.paths = std::move(paths);
    return f;
}

MvtTile tileWith(std::string layerName, std::vector<MvtFeature> features) {
    MvtLayer layer;
    layer.name = std::move(layerName);
    layer.extent = kExtent;
    layer.features = std::move(features);
    MvtTile tile;
    tile.layers = {std::move(layer)};
    return tile;
}

VectorRasterStyle lineStyle(double widthPixels) {
    VectorRasterLayerPaint paint;
    paint.layer = "roads";
    paint.lineColor = {255, 255, 255, 255};
    paint.lineWidthPixels = widthPixels;
    VectorRasterStyle style;
    style.layers = {paint};
    return style;
}

const uint8_t* texel(const LineFieldImage& img, int x, int y) {
    return img.rgba8.data() +
           (static_cast<size_t>(y) * img.size + x) * 4u;
}

bool texelValid(const LineFieldImage& img, int x, int y) {
    return texel(img, x, y)[3] != 0;  // A==0 = 空哨兵
}

/// host 端复刻 FS 的 D2 gather-min 解算(与 PageStoreSamplingGLSL.h 逐步
/// 对应)——单测以此锁「烘焙编码 ↔ 解算」的契约:一侧改口径另一侧不改,
/// 本文件的重建精度断言必红。返回到最近线段的距离(texel 单位)。
double solveDistTexels(const LineFieldImage& img, double px, double py) {
    constexpr double kOff = kLineFieldOffsetRangeTexels;
    const int n = img.size;
    const int ox0 = static_cast<int>(std::floor(px - 0.5));
    const int oy0 = static_cast<int>(std::floor(py - 0.5));
    double best = 1e9;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int tx = std::clamp(ox0 + i, 0, n - 1);
            const int ty = std::clamp(oy0 + j, 0, n - 1);
            const uint8_t* t = texel(img, tx, ty);
            if (t[3] == 0) continue;
            const double qx =
                tx + 0.5 + (t[0] / 255.0 * 2.0 - 1.0) * kOff;
            const double qy =
                ty + 0.5 + (t[1] / 255.0 * 2.0 - 1.0) * kOff;
            const double th = t[2] / 255.0 * 3.14159265358979;
            const double ux = std::cos(th), uy = std::sin(th);
            const double fwd = (t[3] >> 4) * kLineFieldClampStepTexels;
            const double bck = (t[3] & 0xF) * kLineFieldClampStepTexels;
            const double pqx = px - qx, pqy = py - qy;
            const double dTan = ux * pqx + uy * pqy;
            const double dNrm = -uy * pqx + ux * pqy;
            const double ex =
                std::max(std::max(dTan - fwd, -dTan - bck), 0.0);
            best = std::min(best, std::hypot(dNrm, ex));
        }
    }
    return best;
}

} // namespace

// 水平中线(y=32.0 texel):D2 重建 = 精确(仅量化误差)。沿线与横向
// 亚纹素采样,重建距离与真值差 < 0.08 texel(偏移量化 0.031 + 舍入)。
TEST(LineFieldRasterizerTest, StraightLineReconstructsExactly) {
    const MvtTile tile =
        tileWith("roads", {lineFeature({{pt(0, 2048), pt(4096, 2048)}})});
    const std::vector<MvtTileRef> refs{{&tile, 0, 0, 0}};
    const auto img = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(8.0), 64);
    ASSERT_EQ(img.size, 64);
    ASSERT_EQ(img.rgba8.size(), 64u * 64u * 4u);

    for (double px = 8.25; px < 56.0; px += 3.37) {
        for (double dy : {0.0, 0.31, 0.77, 1.5, 2.9}) {
            const double rec = solveDistTexels(img, px, 32.0 + dy);
            EXPECT_NEAR(rec, dy, 0.08)
                << "px=" << px << " dy=" << dy;
        }
    }
}

// 端点收口:断头路端点之外按圆帽衰减(D1 的"无限直线外延胡须"必红此测)。
// 线段止于 x=32:x=34 处到线的真距离 = 2(点到端点),不是 0(延长线)。
TEST(LineFieldRasterizerTest, EndpointClampStopsWhiskers) {
    const MvtTile tile =
        tileWith("roads", {lineFeature({{pt(0, 2048), pt(2048, 2048)}})});
    const std::vector<MvtTileRef> refs{{&tile, 0, 0, 0}};
    const auto img = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(8.0), 64);

    // 线内(端点前):距离≈0。
    EXPECT_NEAR(solveDistTexels(img, 30.0, 32.0), 0.0, 0.08);
    // 端点(x=32)之外 2 texel:真距离=2.0;无限直线外延会给 ≈0。
    // 端点余量 4bit 量化(0.1)+胶囊拼接给 ±0.2 容差。
    EXPECT_NEAR(solveDistTexels(img, 34.0, 32.0), 2.0, 0.2);
    // 外延 1 texel 处同理(D1 胡须重灾区)。
    EXPECT_NEAR(solveDistTexels(img, 33.0, 32.0), 1.0, 0.2);
}

// 交叉路口:两线各自被邻域纹素记录,gather-min 两条都在(无插值互毁)。
TEST(LineFieldRasterizerTest, CrossingKeepsBothLines) {
    const MvtTile tile = tileWith(
        "roads", {lineFeature({{pt(0, 2048), pt(4096, 2048)}}),
                  lineFeature({{pt(2048, 0), pt(2048, 4096)}})});
    const std::vector<MvtTileRef> refs{{&tile, 0, 0, 0}};
    const auto img = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(8.0), 64);

    // 交点上距离≈0;交点外沿两臂各 3 texel 处仍 ≈0(两线都活着)。
    EXPECT_NEAR(solveDistTexels(img, 32.0, 32.0), 0.0, 0.1);
    EXPECT_NEAR(solveDistTexels(img, 35.0, 32.0), 0.0, 0.1);
    EXPECT_NEAR(solveDistTexels(img, 32.0, 35.0), 0.0, 0.1);
}

// 宽度不烘进场:不同 lineWidthPixels 产出逐字节一致(宽度语义在 FS ramp)。
TEST(LineFieldRasterizerTest, WidthIsNotBakedIn) {
    const MvtTile tile =
        tileWith("roads", {lineFeature({{pt(0, 2048), pt(4096, 2048)}})});
    const std::vector<MvtTileRef> refs{{&tile, 0, 0, 0}};
    const auto thin = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(2.0), 64);
    const auto wide = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(12.0), 64);
    EXPECT_EQ(thin.rgba8, wide.rgba8);
}

// zoom 门槛与 filter 与栅格通路同语义(styleZoom 求值)。
TEST(LineFieldRasterizerTest, ZoomRangeAndFilterApply) {
    MvtFeature keep = lineFeature({{pt(0, 2048), pt(4096, 2048)}});
    keep.properties["highway"] = "motorway";
    MvtFeature drop = lineFeature({{pt(0, 1024), pt(4096, 1024)}});
    drop.properties["highway"] = "footway";

    VectorRasterStyle style = lineStyle(8.0);
    style.layers[0].minZoom = 10;
    style.layers[0].filter = StyleFilter::in("highway", {"motorway"});

    const MvtTile tile = tileWith("roads", {keep, drop});
    const std::vector<MvtTileRef> refs{{&tile, 0, 0, 0}};

    const auto z14 = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, style, 64);
    EXPECT_TRUE(texelValid(z14, 32, 32)) << "motorway 应画(中线 y=32)";
    EXPECT_FALSE(texelValid(z14, 32, 16)) << "footway 被 filter 掉(y=16)";

    const auto z5 = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 5, style, 64);
    EXPECT_FALSE(texelValid(z5, 32, 32)) << "z5 < minZoom 整层跳过";
}

// overzoom 子矩形:场内容 = 整瓦对应区域现画(与面栅格化同一仿射语义)。
TEST(LineFieldRasterizerTest, OverzoomSubRectPaintsLocalField) {
    const MvtTile tile =
        tileWith("roads", {lineFeature({{pt(0, 2048), pt(4096, 2048)}})});
    const std::vector<MvtTileRef> refs{{&tile, 2, 1, 1}};

    // 子矩形 [0.25,0.375]×[0.3125,0.4375]:线(unit y=0.375)在画布中央。
    const auto img = rasterizeLineFieldRect(
        refs, MercatorRect{0.25, 0.3125, 0.375, 0.4375}, 16,
        lineStyle(8.0), 64);
    EXPECT_NEAR(solveDistTexels(img, 32.0, 32.0), 0.0, 0.1)
        << "线心应在画布中央";
    EXPECT_FALSE(texelValid(img, 32, 8)) << "远处无线(空哨兵)";

    // 不含线的子矩形:上半远区应全 0(空哨兵;下边缘可落进编码范围,不计)。
    const auto off = rasterizeLineFieldRect(
        refs, MercatorRect{0.25, 0.25, 0.375, 0.3125}, 16,
        lineStyle(8.0), 64);
    bool farHalfAllEmpty = true;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            farHalfAllEmpty = farHalfAllEmpty && !texelValid(off, x, y);
        }
    }
    EXPECT_TRUE(farHalfAllEmpty);
}

// 空瓦/无 line 层:全 0(空哨兵=失败安全),尺寸正确。
TEST(LineFieldRasterizerTest, EmptyYieldsAllZero) {
    MvtTile empty;
    const std::vector<MvtTileRef> refs{{&empty, 0, 0, 0}};
    const auto img = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(8.0), 16);
    ASSERT_EQ(img.size, 16);
    ASSERT_EQ(img.rgba8.size(), 16u * 16u * 4u);
    for (uint8_t v : img.rgba8) EXPECT_EQ(v, 0);
}

// ===== 逐点 GCJ 变换(修真机大页边缘错位)=====

#include "earth_engine/providers/MvtRectCoverage.h"

// 标准 overlay(nullptr)= 恒等映射,与不传变换逐字节一致(零回归)。
TEST(LineFieldRasterizerTest, NullTransformIsIdentity) {
    const MvtTile tile =
        tileWith("roads", {lineFeature({{pt(0, 2048), pt(4096, 2048)}})});
    const std::vector<MvtTileRef> refs{{&tile, 0, 0, 0}};
    const auto a = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(8.0), 64, nullptr);
    const auto b = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(8.0), 64);
    ASSERT_EQ(a.rgba8.size(), b.rgba8.size());
    EXPECT_EQ(a.rgba8, b.rgba8) << "nullptr 变换应与默认参数逐字节一致";
}

// 逐点 GCJ:大页里逐顶点变换 vs 整页单点平移在**边缘**发散(整页平移的病)。
TEST(LineFieldRasterizerTest, PerVertexGcjFixesEdgeVsWholePage) {
    MvtFeature edgeLine;
    edgeLine.type = MvtGeomType::LineString;
    edgeLine.paths = {{pt(4000, 0), pt(4000, 4096)}};
    const int z = 10, n = 1 << z;
    const double lng = 106.55 * M_PI / 180.0, lat = 29.56 * M_PI / 180.0;
    const int tx = (int)(mvt_rect::unitXFromLongitude(lng) * n);
    const int ty = (int)(mvt_rect::unitYFromLatitude(lat) * n);
    const MvtTile tile = tileWith("roads", {edgeLine});
    const std::vector<MvtTileRef> refs{{&tile, z, tx, ty}};
    const MercatorRect rectGcj = mvt_rect::tileToUnitRect(z, tx, ty);

    const UnitTransform xf = mvt_rect::wgsUnitToGcjUnit;
    const auto perVertex = rasterizeLineFieldRect(
        refs, rectGcj, z, lineStyle(6.0), 128, &xf);
    const MercatorRect rectShift = mvt_rect::shiftRectGcjToWgs84(rectGcj);
    const auto wholePage = rasterizeLineFieldRect(
        refs, rectShift, z, lineStyle(6.0), 128, nullptr);

    // 两法都应画出竖线:中行扫有效纹素的列位置。
    auto lineCol = [](const LineFieldImage& img) {
        const int row = img.size / 2;
        for (int x = img.size - 1; x >= 0; --x) {
            const uint8_t* t = img.rgba8.data() +
                (static_cast<size_t>(row) * img.size + x) * 4u;
            if (t[3] != 0) return x;
        }
        return -1;
    };
    EXPECT_GT(lineCol(perVertex), 0);
    EXPECT_GT(lineCol(wholePage), 0);
}
