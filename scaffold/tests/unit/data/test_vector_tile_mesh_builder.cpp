#include <gtest/gtest.h>

#include "earth_engine/data/VectorTileMeshBuilder.h"

#include <cmath>
#include <string>

using namespace earth_engine;

namespace {

MvtTile makeTile(const std::string& layerName, MvtGeomType type,
                 std::vector<std::vector<MvtPoint>> paths,
                 uint32_t extent = 4096) {
    MvtFeature feature;
    feature.id = 1;
    feature.type = type;
    feature.paths = std::move(paths);

    MvtLayer layer;
    layer.name = layerName;
    layer.extent = extent;
    layer.features = {feature};

    MvtTile tile;
    tile.layers = {layer};
    return tile;
}

/// 覆盖瓦片左半边的矩形(extent 空间)。
std::vector<MvtPoint> halfTileRing(int extent = 4096) {
    return {{0, 0}, {extent / 2, 0}, {extent / 2, extent}, {0, extent}};
}

VectorRasterStyle fillStyle(const std::string& layerName) {
    VectorRasterLayerPaint paint;
    paint.layer = layerName;
    paint.fillColor = {10, 20, 30, 255};
    VectorRasterStyle style;
    style.layers = {paint};
    return style;
}

VectorRasterStyle lineStyle(const std::string& layerName, double width) {
    VectorRasterLayerPaint paint;
    paint.layer = layerName;
    paint.lineColor = {200, 100, 50, 255};
    paint.lineWidthPixels = width;
    VectorRasterStyle style;
    style.layers = {paint};
    return style;
}

}  // namespace

// 面被三角化,且顶点落在**瓦片本地归一化** [0,1] 里 —— 归一化是 overzoom 红利的
// 前提:页只换正交矩阵的子矩形,网格不动。
TEST(VectorTileMeshBuilder, PolygonTessellatesIntoNormalizedSpace) {
    MvtTile tile = makeTile("L", MvtGeomType::Polygon, {halfTileRing()});
    VectorTileMesh mesh = buildVectorTileMesh(tile, 12, fillStyle("L"));

    ASSERT_FALSE(mesh.empty());
    EXPECT_EQ(mesh.indices.size() % 3, 0u);
    for (const VectorTileMeshVertex& v : mesh.vertices) {
        EXPECT_GE(v.x, 0.0f);
        EXPECT_LE(v.x, 1.0f);
        EXPECT_GE(v.y, 0.0f);
        EXPECT_LE(v.y, 1.0f);
        EXPECT_EQ(v.ex, 0.0f) << "面不挤压";
        EXPECT_EQ(v.ey, 0.0f);
    }
}

// **extent 是 per-layer 的**,归一化必须按各层自己的 extent 算。写死 4096 的话
// 非 4096 的图层会整体缩放错位 —— 真机表现是「某一层的路网偏移/超出瓦片」。
TEST(VectorTileMeshBuilder, NormalizesByPerLayerExtent) {
    MvtTile tile = makeTile("L", MvtGeomType::Polygon, {halfTileRing(1024)},
                            /*extent=*/1024);
    VectorTileMesh mesh = buildVectorTileMesh(tile, 12, fillStyle("L"));

    ASSERT_FALSE(mesh.empty());
    float maxX = 0.0f;
    for (const VectorTileMeshVertex& v : mesh.vertices) {
        maxX = std::max(maxX, v.x);
    }
    EXPECT_NEAR(maxX, 0.5f, 1e-5f) << "extent=1024 下 x=512 应归一到 0.5";
}

// 孔环必须被挖掉:CDT 按约束边奇偶 flood-fill 判内外。若孔没挖掉,三角形数会
// 接近实心矩形 —— 用「有三角形但覆盖面积不满」间接判定。
TEST(VectorTileMeshBuilder, HoleIsCarvedOut) {
    const std::vector<MvtPoint> outer = {{0, 0}, {4000, 0}, {4000, 4000}, {0, 4000}};
    const std::vector<MvtPoint> hole = {{1000, 1000}, {1000, 3000},
                                        {3000, 3000}, {3000, 1000}};
    MvtTile solid = makeTile("L", MvtGeomType::Polygon, {outer});
    MvtTile holed = makeTile("L", MvtGeomType::Polygon, {outer, hole});

    auto area = [](const VectorTileMesh& m) {
        double sum = 0.0;
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const VectorTileMeshVertex& a = m.vertices[m.indices[i]];
            const VectorTileMeshVertex& b = m.vertices[m.indices[i + 1]];
            const VectorTileMeshVertex& c = m.vertices[m.indices[i + 2]];
            sum += std::fabs((static_cast<double>(b.x) - a.x) * (c.y - a.y) -
                             (static_cast<double>(c.x) - a.x) * (b.y - a.y)) *
                   0.5;
        }
        return sum;
    };
    const double solidArea = area(buildVectorTileMesh(solid, 12, fillStyle("L")));
    const double holedArea = area(buildVectorTileMesh(holed, 12, fillStyle("L")));
    ASSERT_GT(solidArea, 0.0);
    // 孔占外环面积的 (2000/4000)² = 1/4。
    EXPECT_NEAR(holedArea / solidArea, 0.75, 0.02);
}

// **线宽存进 extrude 而不是烘进位置** —— 这是「一份网格服务所有页 zoom」的支点。
// 位置必须与线宽无关:改线宽只该改 extrude,顶点坐标一个字节都不许动。
TEST(VectorTileMeshBuilder, LineWidthLivesInExtrudeNotPosition) {
    MvtTile tile = makeTile("L", MvtGeomType::LineString,
                            {{{0, 2048}, {4096, 2048}}});
    VectorTileMesh thin = buildVectorTileMesh(tile, 12, lineStyle("L", 2.0));
    VectorTileMesh thick = buildVectorTileMesh(tile, 12, lineStyle("L", 8.0));

    ASSERT_EQ(thin.vertices.size(), thick.vertices.size());
    ASSERT_FALSE(thin.empty());
    bool sawExtrude = false;
    for (size_t i = 0; i < thin.vertices.size(); ++i) {
        EXPECT_EQ(thin.vertices[i].x, thick.vertices[i].x) << "位置不许随线宽变";
        EXPECT_EQ(thin.vertices[i].y, thick.vertices[i].y);
        if (std::fabs(thin.vertices[i].ex) > 0.0f ||
            std::fabs(thin.vertices[i].ey) > 0.0f) {
            sawExtrude = true;
            EXPECT_NEAR(std::fabs(thick.vertices[i].ex),
                        std::fabs(thin.vertices[i].ex) * 4.0f, 1e-4f);
            EXPECT_NEAR(std::fabs(thick.vertices[i].ey),
                        std::fabs(thin.vertices[i].ey) * 4.0f, 1e-4f);
        }
    }
    EXPECT_TRUE(sawExtrude) << "线必须产出非零挤压";
}

// 挤压方向必须垂直于线段。方向错了(比如用了切向)线会沿自己延伸而不是变宽,
// 真机表现是「线消失或变成一串点」。
TEST(VectorTileMeshBuilder, ExtrudeIsPerpendicularToSegment) {
    // 水平段 → 挤压应为纯 y 向。
    MvtTile tile = makeTile("L", MvtGeomType::LineString,
                            {{{0, 2048}, {4096, 2048}}});
    VectorTileMesh mesh = buildVectorTileMesh(tile, 12, lineStyle("L", 4.0));
    ASSERT_FALSE(mesh.empty());
    // 前 4 个顶点是首段的四边形(接头方块排在所有段之后)。
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_NEAR(mesh.vertices[i].ex, 0.0f, 1e-5f);
        EXPECT_NEAR(std::fabs(mesh.vertices[i].ey), 2.0f, 1e-5f);
    }
}

// 画家序:同一层内先面后线(线压在面上),与 E4-1 栅格路径同序。两条路径压盖
// 关系不一致的话,cell 在页存储与 directComposite 之间切换时画面会跳。
TEST(VectorTileMeshBuilder, FillEmittedBeforeLineWithinLayer) {
    MvtTile tile = makeTile("L", MvtGeomType::Polygon, {halfTileRing()});
    VectorRasterLayerPaint paint;
    paint.layer = "L";
    paint.fillColor = {1, 2, 3, 255};
    paint.lineColor = {9, 8, 7, 255};
    paint.lineWidthPixels = 3.0;
    VectorRasterStyle style;
    style.layers = {paint};

    VectorTileMesh mesh = buildVectorTileMesh(tile, 12, style);
    ASSERT_FALSE(mesh.empty());
    // 第一个索引指向的顶点应是 fill 色;最后一个应是 line 色。
    EXPECT_EQ(mesh.vertices[mesh.indices.front()].r, 1);
    EXPECT_EQ(mesh.vertices[mesh.indices.back()].r, 9);
}

// zoom 区间与 alpha=0 都表示「不绘制该通道」,不是画透明。
TEST(VectorTileMeshBuilder, ZoomRangeAndZeroAlphaSkipChannel) {
    MvtTile tile = makeTile("L", MvtGeomType::Polygon, {halfTileRing()});

    VectorRasterStyle outOfRange = fillStyle("L");
    outOfRange.layers[0].minZoom = 14;
    EXPECT_TRUE(buildVectorTileMesh(tile, 12, outOfRange).empty());

    VectorRasterStyle transparent = fillStyle("L");
    transparent.layers[0].fillColor = {10, 20, 30, 0};
    EXPECT_TRUE(buildVectorTileMesh(tile, 12, transparent).empty());
}

// 样式里没有的图层、退化环、零长段都不该产出垃圾三角形或崩。
TEST(VectorTileMeshBuilder, DegenerateInputsProduceNothing) {
    EXPECT_TRUE(
        buildVectorTileMesh(makeTile("other", MvtGeomType::Polygon,
                                     {halfTileRing()}),
                            12, fillStyle("L"))
            .empty());
    EXPECT_TRUE(buildVectorTileMesh(
                    makeTile("L", MvtGeomType::Polygon, {{{0, 0}, {1, 1}}}), 12,
                    fillStyle("L"))
                    .empty()) << "点数不足的环";
    // 全零长段的折线:段全跳过,但接头方块仍会画(等价于一个点标记),不该崩。
    VectorTileMesh mesh = buildVectorTileMesh(
        makeTile("L", MvtGeomType::LineString, {{{5, 5}, {5, 5}}}), 12,
        lineStyle("L", 2.0));
    EXPECT_EQ(mesh.indices.size() % 3, 0u);
}
