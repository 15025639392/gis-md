#include "earth_engine/data/MvtFeatureConverter.h"

#include "earth_engine/tiling/TileScheme.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace earth_engine;

namespace {

TileKey makeKey(int z, int x, int y) {
    return TileKey{SchemeId("XYZ-WebMercator"), z, x, y};
}

// ---------------------------------------------------------------------------
// 坐标转换:对拍 TileScheme::tileToRectangle 的瓦片角点
// ---------------------------------------------------------------------------

TEST(MvtFeatureConverter, TileCornersMatchTileScheme) {
    auto scheme = TileScheme::createXYZWebMercator();
    const uint32_t extent = 4096;
    for (const TileKey& key :
         {makeKey(0, 0, 0), makeKey(3, 2, 5), makeKey(11, 1685, 857)}) {
        Rectangle rect = scheme->tileToRectangle(key);
        // (0,0) = 西北角;(extent,extent) = 东南角(y 向下)
        Cartographic nw = mvtToCartographic(
            key, extent, MvtPoint{0, 0});
        Cartographic se = mvtToCartographic(
            key, extent,
            MvtPoint{static_cast<int32_t>(extent), static_cast<int32_t>(extent)});
        EXPECT_NEAR(nw.longitude(), rect.west(), 1e-12) << key;
        EXPECT_NEAR(nw.latitude(), rect.north(), 1e-9) << key;
        EXPECT_NEAR(se.longitude(), rect.east(), 1e-12) << key;
        EXPECT_NEAR(se.latitude(), rect.south(), 1e-9) << key;
    }
}

TEST(MvtFeatureConverter, LatitudeIsMercatorNonlinearInsideTile) {
    // z1 北半球瓦片:瓦片中点的纬度必须是 mercator 反解,
    // 不等于矩形纬度中点(线性插值会漂移)
    auto scheme = TileScheme::createXYZWebMercator();
    TileKey key = makeKey(1, 0, 0);
    Rectangle rect = scheme->tileToRectangle(key);
    Cartographic mid = mvtToCartographic(key, 4096, MvtPoint{2048, 2048});
    double linearMid = (rect.south() + rect.north()) * 0.5;
    EXPECT_GT(std::abs(mid.latitude() - linearMid), 1e-3);
    // 反解正确值:全球归一化 y=0.25 → lat = 2·atan(e^(π/2)) − π/2 ≈ 66.51°
    double expected = 2.0 * std::atan(std::exp(M_PI * 0.5)) - M_PI / 2.0;
    EXPECT_NEAR(mid.latitude(), expected, 1e-12);
}

TEST(MvtFeatureConverter, BufferCoordinatesExtrapolate) {
    TileKey key = makeKey(2, 1, 1);
    auto scheme = TileScheme::createXYZWebMercator();
    Rectangle rect = scheme->tileToRectangle(key);
    Cartographic outside = mvtToCartographic(key, 4096, MvtPoint{-128, -128});
    EXPECT_LT(outside.longitude(), rect.west());
    EXPECT_GT(outside.latitude(), rect.north());
}

// ---------------------------------------------------------------------------
// Feature 转换
// ---------------------------------------------------------------------------

MvtLayer makeLayer() {
    MvtLayer layer;
    layer.name = "roads";
    layer.extent = 4096;
    return layer;
}

TEST(MvtFeatureConverter, MultiGeometriesSplitIntoFeatures) {
    MvtLayer layer = makeLayer();

    MvtFeature multiPoint;
    multiPoint.id = 5;
    multiPoint.type = MvtGeomType::Point;
    multiPoint.paths = {{{100, 100}, {200, 200}}};
    layer.features.push_back(multiPoint);

    MvtFeature multiLine;
    multiLine.type = MvtGeomType::LineString;
    multiLine.paths = {{{0, 0}, {10, 10}}, {{20, 20}, {30, 30}, {40, 40}}};
    layer.features.push_back(multiLine);

    auto features = mvtLayerToFeatures(layer, makeKey(5, 10, 12));
    ASSERT_EQ(features.size(), 4u);
    EXPECT_EQ(features[0].type, GeometryType::Point);
    EXPECT_EQ(features[0].sourceId, "5");
    EXPECT_EQ(features[1].type, GeometryType::Point);
    ASSERT_EQ(features[0].rings.size(), 1u);
    EXPECT_EQ(features[0].rings[0].size(), 1u);
    EXPECT_EQ(features[2].type, GeometryType::LineString);
    EXPECT_TRUE(features[2].sourceId.empty());  // id=0 → 空
    EXPECT_EQ(features[2].rings[0].size(), 2u);
    EXPECT_EQ(features[3].rings[0].size(), 3u);
}

TEST(MvtFeatureConverter, PolygonWithHoleClassified) {
    MvtLayer layer = makeLayer();
    MvtFeature poly;
    poly.type = MvtGeomType::Polygon;
    poly.paths = {
        {{0, 0}, {100, 0}, {100, 100}, {0, 100}},   // 外环
        {{20, 20}, {20, 80}, {80, 80}, {80, 20}},   // 孔(反向)
        {{200, 200}, {300, 200}, {300, 300}, {200, 300}},  // 第二个面
    };
    layer.features.push_back(poly);

    auto features = mvtLayerToFeatures(layer, makeKey(5, 10, 12));
    ASSERT_EQ(features.size(), 2u);
    EXPECT_EQ(features[0].type, GeometryType::Polygon);
    EXPECT_EQ(features[0].rings.size(), 2u);  // 外环 + 孔
    EXPECT_EQ(features[1].rings.size(), 1u);
}

TEST(MvtFeatureConverter, PropertiesCopiedAndLayerNameInjected) {
    MvtLayer layer = makeLayer();
    MvtFeature line;
    line.type = MvtGeomType::LineString;
    line.paths = {{{0, 0}, {10, 10}}, {{20, 20}, {30, 30}}};
    line.properties = {{"highway", "primary"}};
    layer.features.push_back(line);

    auto features = mvtLayerToFeatures(layer, makeKey(5, 10, 12));
    ASSERT_EQ(features.size(), 2u);
    for (const Feature& f : features) {
        EXPECT_EQ(f.properties.at("highway"), "primary");
        EXPECT_EQ(f.properties.at("mvt_layer"), "roads");
        EXPECT_EQ(f.id, kInvalidFeatureId);
    }
}

TEST(MvtFeatureConverter, DegenerateAndUnknownDropped) {
    MvtLayer layer = makeLayer();
    MvtFeature shortLine;
    shortLine.type = MvtGeomType::LineString;
    shortLine.paths = {{{0, 0}}};  // 1 点线,退化
    layer.features.push_back(shortLine);
    MvtFeature unknown;
    unknown.type = MvtGeomType::Unknown;
    unknown.paths = {{{0, 0}, {10, 10}}};
    layer.features.push_back(unknown);

    auto features = mvtLayerToFeatures(layer, makeKey(5, 10, 12));
    EXPECT_TRUE(features.empty());
}

} // namespace
