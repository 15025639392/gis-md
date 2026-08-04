#include <gtest/gtest.h>

#include "earth_engine/data/VectorTileRasterizer.h"

using namespace earth_engine;

namespace {

constexpr uint32_t kExtent = 4096;

MvtPoint pt(int x, int y) { return MvtPoint{x, y}; }

/// 覆盖瓦片左半边的矩形环(顺时针)。
MvtFeature halfTilePolygon() {
    MvtFeature f;
    f.type = MvtGeomType::Polygon;
    f.paths = {{pt(0, 0), pt(2048, 0), pt(2048, 4096), pt(0, 4096)}};
    return f;
}

/// 带孔的矩形:外环覆盖全瓦,孔在中心(绕向相反 → nonzero 挖掉)。
MvtFeature polygonWithHole() {
    MvtFeature f;
    f.type = MvtGeomType::Polygon;
    f.paths = {
        {pt(0, 0), pt(4096, 0), pt(4096, 4096), pt(0, 4096)},          // 外环 CW
        {pt(1024, 1024), pt(1024, 3072), pt(3072, 3072), pt(3072, 1024)},  // 孔 CCW
    };
    return f;
}

MvtFeature horizontalLine() {
    MvtFeature f;
    f.type = MvtGeomType::LineString;
    f.paths = {{pt(0, 2048), pt(4096, 2048)}};
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

struct Rgba { int r, g, b, a; };

Rgba pixel(const VectorRasterImage& img, int x, int y) {
    const uint8_t* p =
        img.rgba.data() + (static_cast<size_t>(y) * img.size + x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

VectorRasterStyle fillStyle(std::array<uint8_t, 4> color) {
    VectorRasterLayerPaint paint;
    paint.layer = "L";
    paint.fillColor = color;
    VectorRasterStyle style;
    style.layers = {paint};
    style.supersample = 2;
    return style;
}

} // namespace

// 最基本的正确性:填对了区域,而不是"看着像"。左半填、右半空。
TEST(VectorTileRasterizerTest, PolygonFillsCorrectHalf) {
    const MvtTile tile = tileWith("L", {halfTilePolygon()});
    const auto img =
        rasterizeMvtTile(tile, 12, fillStyle({255, 0, 0, 255}), 64);
    ASSERT_EQ(img.size, 64);

    const Rgba left = pixel(img, 16, 32);
    EXPECT_EQ(left.r, 255);
    EXPECT_EQ(left.a, 255);
    const Rgba right = pixel(img, 48, 32);
    EXPECT_EQ(right.a, 0) << "右半应保持背景(全透明)";
}

// 孔环靠 nonzero 的反向绕向自动挖掉。用 even-odd 也能过这条,但 even-odd
// 在自相交多边形上会挖出错误的洞,而 OSM 数据里自相交并不罕见。
TEST(VectorTileRasterizerTest, HoleIsCarvedOut) {
    const MvtTile tile = tileWith("L", {polygonWithHole()});
    const auto img =
        rasterizeMvtTile(tile, 12, fillStyle({0, 255, 0, 255}), 64);

    EXPECT_EQ(pixel(img, 4, 32).a, 255) << "外环内应填充";
    EXPECT_EQ(pixel(img, 32, 32).a, 0) << "孔中心应挖空";
}

TEST(VectorTileRasterizerTest, LineStrokeHasWidth) {
    VectorRasterLayerPaint paint;
    paint.layer = "L";
    paint.lineColor = {0, 0, 255, 255};
    paint.lineWidthPixels = 4.0;
    VectorRasterStyle style;
    style.layers = {paint};

    const MvtTile tile = tileWith("L", {horizontalLine()});
    const auto img = rasterizeMvtTile(tile, 12, style, 64);

    EXPECT_GT(pixel(img, 32, 32).b, 200) << "线心应着色";
    EXPECT_EQ(pixel(img, 32, 8).a, 0) << "远离线的地方不该着色";
    // 线宽 4px → 中心上下各 ~2px 内应有覆盖。
    EXPECT_GT(pixel(img, 32, 31).a, 0);
    EXPECT_GT(pixel(img, 32, 33).a, 0);
}

// alpha=0 表示**不绘制该通道**,而不是"画一层透明"。分不清的话,只想描边
// 的图层会被一层透明 fill 盖掉底下的内容。
TEST(VectorTileRasterizerTest, ZeroAlphaMeansSkipNotDraw) {
    VectorRasterLayerPaint paint;
    paint.layer = "L";
    paint.fillColor = {255, 0, 0, 0};  // 不画 fill
    paint.lineColor = {0, 0, 255, 255};
    paint.lineWidthPixels = 4.0;
    VectorRasterStyle style;
    style.layers = {paint};

    const MvtTile tile = tileWith("L", {halfTilePolygon()});
    const auto img = rasterizeMvtTile(tile, 12, style, 64);
    // polygon 的**外环**照样被描边(底图里 building outline 就靠这个),
    // 但内部不该被填成红色。
    EXPECT_EQ(pixel(img, 8, 32).r, 0) << "fill alpha=0 → 内部不该有红";
}

// 图层 zoom 区间与 E2 的 filter 共用同一套语义,这里钉住栅格路径也遵守。
TEST(VectorTileRasterizerTest, LayerZoomRangeAndFilterApply) {
    MvtFeature f = halfTilePolygon();
    f.properties["kind"] = "keep";
    MvtFeature g = halfTilePolygon();
    g.properties["kind"] = "drop";

    VectorRasterLayerPaint paint;
    paint.layer = "L";
    paint.fillColor = {255, 0, 0, 255};
    paint.minZoom = 10;
    paint.maxZoom = 14;
    paint.filter = StyleFilter::compare("kind", StyleFilter::Compare::Equal,
                                        std::string("keep"));
    VectorRasterStyle style;
    style.layers = {paint};

    const MvtTile tile = tileWith("L", {f, g});
    EXPECT_EQ(pixel(rasterizeMvtTile(tile, 12, style, 64), 16, 32).a, 255)
        << "z12 在区间内且有匹配要素";
    EXPECT_EQ(pixel(rasterizeMvtTile(tile, 5, style, 64), 16, 32).a, 0)
        << "z5 在区间外 → 整层跳过";

    // 过滤掉全部要素时应产出空图(而非崩)。
    paint.filter = StyleFilter::compare("kind", StyleFilter::Compare::Equal,
                                        std::string("none"));
    style.layers = {paint};
    EXPECT_EQ(pixel(rasterizeMvtTile(tile, 12, style, 64), 16, 32).a, 0);
}

// 同层内相邻线段自重叠不得叠出暗斑 —— 覆盖用 mask 而非逐段混合就是为这个。
TEST(VectorTileRasterizerTest, SelfOverlapDoesNotDarken) {
    MvtFeature zigzag;
    zigzag.type = MvtGeomType::LineString;
    zigzag.paths = {{pt(512, 2048), pt(2048, 2048), pt(3584, 2048)}};

    VectorRasterLayerPaint paint;
    paint.layer = "L";
    paint.lineColor = {200, 200, 200, 128};  // 半透明:叠加会明显变亮/变暗
    paint.lineWidthPixels = 8.0;
    VectorRasterStyle style;
    style.layers = {paint};
    style.supersample = 1;  // 关 AA,免得降采样掩盖差异

    const auto img = rasterizeMvtTile(tileWith("L", {zigzag}), 12, style, 64);
    const Rgba joint = pixel(img, 32, 32);   // 两段接头处
    const Rgba mid = pixel(img, 20, 32);     // 单段中部
    EXPECT_EQ(joint.r, mid.r) << "接头不得比单段更亮/更暗";
    EXPECT_EQ(joint.a, mid.a);
}

TEST(VectorTileRasterizerTest, EmptyTileYieldsBackground) {
    MvtTile empty;
    VectorRasterStyle style = fillStyle({255, 0, 0, 255});
    style.background = {10, 20, 30, 255};
    const auto img = rasterizeMvtTile(empty, 12, style, 16);
    ASSERT_EQ(img.size, 16);
    const Rgba p = pixel(img, 8, 8);
    EXPECT_EQ(p.r, 10);
    EXPECT_EQ(p.g, 20);
    EXPECT_EQ(p.b, 30);
    EXPECT_EQ(p.a, 255);
}
