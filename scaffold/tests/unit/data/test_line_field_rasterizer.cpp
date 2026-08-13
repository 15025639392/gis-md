#include <gtest/gtest.h>

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

uint8_t at(const LineFieldImage& img, int x, int y) {
    return img.r8[static_cast<size_t>(y) * img.size + x];
}

/// 编码逆变换:field byte → 有符号边缘距离(texel)。反向编码:0=远。
double sdOf(uint8_t v) {
    return (0.5 - v / 255.0) * 2.0 * kLineFieldFeatherTexels;
}

} // namespace

// 水平中线,宽 8px:线心 sd=-4(=-halfWidth),边缘处≈0,远处 255。
// 场值是**有符号边缘距离**的量化——这是"半宽烘进场"的核心判据。
TEST(LineFieldRasterizerTest, StraightLineEncodesSignedEdgeDistance) {
    const MvtTile tile =
        tileWith("roads", {lineFeature({{pt(0, 2048), pt(4096, 2048)}})});
    const std::vector<MvtTileRef> refs{{&tile, 0, 0, 0}};
    const auto img = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(8.0), 64);
    ASSERT_EQ(img.size, 64);

    // 线心(y=32 即中线):sd ≈ -4(半宽)。量化+像素中心偏差给 ±0.6。
    EXPECT_NEAR(sdOf(at(img, 32, 32)), -4.0, 0.6);
    // 边缘(中线 ±4px):sd ≈ 0 → 值 ≈ 128。
    EXPECT_NEAR(sdOf(at(img, 32, 36)), 0.0, 0.6);
    // 远处:0(窗口外 clamp;0=失败安全的"无线")。
    EXPECT_EQ(at(img, 32, 8), 0);
    // 羽化带内(边缘外 2px):sd ≈ +2。
    EXPECT_NEAR(sdOf(at(img, 32, 38)), 2.0, 0.6);
}

// 交叉路口:两线相交处取 min,不得出现"路口断裂"(值应 ≤ 单线线心值)。
TEST(LineFieldRasterizerTest, CrossingTakesMin) {
    const MvtTile tile = tileWith(
        "roads", {lineFeature({{pt(0, 2048), pt(4096, 2048)}}),
                  lineFeature({{pt(2048, 0), pt(2048, 4096)}})});
    const std::vector<MvtTileRef> refs{{&tile, 0, 0, 0}};
    const auto img = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(8.0), 64);

    const uint8_t crossing = at(img, 32, 32);
    const uint8_t single = at(img, 16, 32);
    EXPECT_GE(crossing, single) << "路口场值不得低于单线(min(sd) = max(编码))";
    EXPECT_GT(crossing, 191) << "路口深在线内";
}

// 不同层不同宽:宽路的"线内带"更宽。
TEST(LineFieldRasterizerTest, PerBandWidthIsBakedIn) {
    const MvtTile tile =
        tileWith("roads", {lineFeature({{pt(0, 2048), pt(4096, 2048)}})});
    const std::vector<MvtTileRef> refs{{&tile, 0, 0, 0}};

    const auto thin = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(2.0), 64);
    const auto wide = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(12.0), 64);
    // 中线 ±3px 处:细线(半宽1)已在外(sd≈+2),宽线(半宽6)仍在内(sd≈-3)。
    EXPECT_GT(sdOf(at(thin, 32, 35)), 1.0);
    EXPECT_LT(sdOf(at(wide, 32, 35)), -1.0);
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
    EXPECT_GT(at(z14, 32, 32), 223) << "motorway 应画(中线在 y=32)";
    EXPECT_EQ(at(z14, 32, 16), 0) << "footway 被 filter 掉(其中线 y=16)";

    const auto z5 = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 5, style, 64);
    EXPECT_EQ(at(z5, 32, 32), 0) << "z5 < minZoom 整层跳过";
}

// overzoom 子矩形:场内容 = 整瓦对应区域现画(与面栅格化同一仿射语义)。
// 瓦 (z2,1,1) 的水平中线在 unit y=0.375;子矩形取该线附近。
TEST(LineFieldRasterizerTest, OverzoomSubRectPaintsLocalField) {
    const MvtTile tile =
        tileWith("roads", {lineFeature({{pt(0, 2048), pt(4096, 2048)}})});
    const std::vector<MvtTileRef> refs{{&tile, 2, 1, 1}};

    // 子矩形 [0.25,0.375]×[0.3125,0.4375]:线(unit y=0.375)在画布中央。
    const auto img = rasterizeLineFieldRect(
        refs, MercatorRect{0.25, 0.3125, 0.375, 0.4375}, 16,
        lineStyle(8.0), 64);
    EXPECT_GT(at(img, 32, 32), 223) << "线心应在画布中央(深在线内)";
    EXPECT_EQ(at(img, 32, 8), 0) << "远处无线";

    // 不含线的子矩形(线在其下边界外):上半远区应全 255
    // (下边缘可能落进羽化带,不计)。
    const auto off = rasterizeLineFieldRect(
        refs, MercatorRect{0.25, 0.25, 0.375, 0.3125}, 16,
        lineStyle(8.0), 64);
    bool farHalfAllZero = true;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 64; ++x) {
            farHalfAllZero = farHalfAllZero && (at(off, x, y) == 0);
        }
    }
    EXPECT_TRUE(farHalfAllZero);
}

// 空瓦/无 line 层:全 255(远离一切线),尺寸正确。
TEST(LineFieldRasterizerTest, EmptyYieldsFarField) {
    MvtTile empty;
    const std::vector<MvtTileRef> refs{{&empty, 0, 0, 0}};
    const auto img = rasterizeLineFieldRect(
        refs, MercatorRect{0, 0, 1, 1}, 14, lineStyle(8.0), 16);
    ASSERT_EQ(img.size, 16);
    for (uint8_t v : img.r8) EXPECT_EQ(v, 0);
}
