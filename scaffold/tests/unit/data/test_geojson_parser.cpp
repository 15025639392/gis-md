#include <gtest/gtest.h>
#include "earth_engine/data/GeoJsonParser.h"
#include "earth_engine/core/geodesy/Transforms.h"

using namespace earth_engine;

// ============================================================
// 基本解析
// ============================================================

TEST(GeoJsonParserTest, EmptyString) {
    auto features = GeoJsonParser::parse("");
    EXPECT_TRUE(features.empty());
}

TEST(GeoJsonParserTest, InvalidJson) {
    auto features = GeoJsonParser::parse("not json");
    EXPECT_TRUE(features.empty());
}

TEST(GeoJsonParserTest, EmptyFeatureCollection) {
    auto features = GeoJsonParser::parse(
        R"({"type":"FeatureCollection","features":[]})");
    EXPECT_TRUE(features.empty());
}

// ============================================================
// Point
// ============================================================

TEST(GeoJsonParserTest, ParsePoint) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","geometry":{"type":"Point","coordinates":[116.397,39.908]}}
        ]}
    )");
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(GeoFeature::Type::Point, features[0].type);
    ASSERT_EQ(1u, features[0].rings.size());
    ASSERT_EQ(1u, features[0].rings[0].size());

    double lngRad = features[0].rings[0][0].longitude();
    double latRad = features[0].rings[0][0].latitude();
    EXPECT_NEAR(Transforms::toRadians(116.397), lngRad, 1e-9);
    EXPECT_NEAR(Transforms::toRadians(39.908), latRad, 1e-9);
}

TEST(GeoJsonParserTest, ParsePointWithHeight) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","geometry":{"type":"Point","coordinates":[116.397,39.908,100.5]}}
        ]}
    )");
    ASSERT_EQ(1u, features.size());
    EXPECT_NEAR(100.5, features[0].rings[0][0].height(), 1e-6);
}

// ============================================================
// LineString
// ============================================================

TEST(GeoJsonParserTest, ParseLineString) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","geometry":{"type":"LineString","coordinates":[
                [116.0,39.0],[117.0,40.0],[118.0,39.5]
            ]}}
        ]}
    )");
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(GeoFeature::Type::LineString, features[0].type);
    ASSERT_EQ(1u, features[0].rings.size());
    EXPECT_EQ(3u, features[0].rings[0].size());
}

// ============================================================
// Polygon
// ============================================================

TEST(GeoJsonParserTest, ParsePolygon) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[
                [116.0,39.0],[117.0,39.0],[117.0,40.0],[116.0,40.0],[116.0,39.0]
            ]]}}
        ]}
    )");
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(GeoFeature::Type::Polygon, features[0].type);
    ASSERT_EQ(1u, features[0].rings.size());
    EXPECT_EQ(5u, features[0].rings[0].size());
}

TEST(GeoJsonParserTest, ParsePolygonWithHole) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","geometry":{"type":"Polygon","coordinates":[
                [[116.0,39.0],[117.0,39.0],[117.0,40.0],[116.0,40.0],[116.0,39.0]],
                [[116.2,39.2],[116.8,39.2],[116.8,39.8],[116.2,39.8],[116.2,39.2]]
            ]}}
        ]}
    )");
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ(GeoFeature::Type::Polygon, features[0].type);
    // 2 rings: outer + hole
    EXPECT_EQ(2u, features[0].rings.size());
    EXPECT_EQ(5u, features[0].rings[0].size());
    EXPECT_EQ(5u, features[0].rings[1].size());
}

// ============================================================
// Properties
// ============================================================

TEST(GeoJsonParserTest, ParseProperties) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","properties":{"name":"Beijing","pop":21540000,"capital":true},
             "geometry":{"type":"Point","coordinates":[116.397,39.908]}}
        ]}
    )");
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ("Beijing", features[0].properties["name"]);
    EXPECT_EQ("21540000.000000", features[0].properties["pop"]);
    EXPECT_EQ("true", features[0].properties["capital"]);
}

// ============================================================
// Feature ID
// ============================================================

TEST(GeoJsonParserTest, FeatureId) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","id":"beijing-1",
             "geometry":{"type":"Point","coordinates":[116.397,39.908]}}
        ]}
    )");
    ASSERT_EQ(1u, features.size());
    EXPECT_EQ("beijing-1", features[0].id);
}

TEST(GeoJsonParserTest, AutoGeneratedId) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","geometry":{"type":"Point","coordinates":[0,0]}},
            {"type":"Feature","geometry":{"type":"Point","coordinates":[1,1]}}
        ]}
    )");
    ASSERT_EQ(2u, features.size());
    EXPECT_EQ("feature-0", features[0].id);
    EXPECT_EQ("feature-1", features[1].id);
}

// ============================================================
// Bounds
// ============================================================

TEST(GeoJsonParserTest, ComputeBounds) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[
                [116.0,39.0],[117.0,39.0],[117.0,40.0],[116.0,40.0],[116.0,39.0]
            ]]}}
        ]}
    )");
    ASSERT_EQ(1u, features.size());
    const auto& b = features[0].bounds;
    EXPECT_NEAR(Transforms::toRadians(116.0), b.west(), 1e-9);
    EXPECT_NEAR(Transforms::toRadians(117.0), b.east(), 1e-9);
    EXPECT_NEAR(Transforms::toRadians(39.0), b.south(), 1e-9);
    EXPECT_NEAR(Transforms::toRadians(40.0), b.north(), 1e-9);
}

// ============================================================
// Multiple features
// ============================================================

TEST(GeoJsonParserTest, MultipleFeatures) {
    auto features = GeoJsonParser::parse(R"(
        {"type":"FeatureCollection","features":[
            {"type":"Feature","geometry":{"type":"Point","coordinates":[116,39]}},
            {"type":"Feature","geometry":{"type":"LineString","coordinates":[[116,39],[117,40]]}},
            {"type":"Feature","geometry":{"type":"Polygon","coordinates":[[[116,39],[117,39],[117,40],[116,40],[116,39]]]}}
        ]}
    )");
    EXPECT_EQ(3u, features.size());
    EXPECT_EQ(GeoFeature::Type::Point, features[0].type);
    EXPECT_EQ(GeoFeature::Type::LineString, features[1].type);
    EXPECT_EQ(GeoFeature::Type::Polygon, features[2].type);
}
