#include <gtest/gtest.h>
#include <cmath>
#include <optional>
#include <utility>
#include "earth_engine/core/math/Rectangle.h"

using namespace earth_engine;

namespace {
constexpr double kDegToRad = M_PI / 180.0;

}

TEST(RectangleTest, ContainsMatchesCesiumNativeSimpleCase) {
    // Ported from cesium-native CesiumGeospatial/test/TestGlobeRectangle.cpp.
    Rectangle simple(0.1, 0.2, 0.3, 0.4);

    EXPECT_TRUE(simple.contains(0.1, 0.2));
    EXPECT_TRUE(simple.contains(0.1, 0.4));
    EXPECT_TRUE(simple.contains(0.3, 0.4));
    EXPECT_TRUE(simple.contains(0.3, 0.2));
    EXPECT_TRUE(simple.contains(0.2, 0.3));
    EXPECT_FALSE(simple.contains(0.0, 0.2));
}

TEST(RectangleTest, EmptyAndMaximumConstantsMatchCesiumNative) {
    // Ported from cesium-native CesiumGeospatial/test/TestGlobeRectangle.cpp:
    // GlobeRectangle::EMPTY and GlobeRectangle::MAXIMUM constants.
    EXPECT_TRUE(Rectangle::EMPTY.isEmpty());
    EXPECT_NEAR(M_PI, Rectangle::EMPTY.west(), 0.0);
    EXPECT_NEAR(M_PI * 0.5, Rectangle::EMPTY.south(), 0.0);
    EXPECT_NEAR(-M_PI, Rectangle::EMPTY.east(), 0.0);
    EXPECT_NEAR(-M_PI * 0.5, Rectangle::EMPTY.north(), 0.0);

    EXPECT_FALSE(Rectangle::MAXIMUM.isEmpty());
    EXPECT_NEAR(-M_PI, Rectangle::MAXIMUM.west(), 0.0);
    EXPECT_NEAR(-M_PI * 0.5, Rectangle::MAXIMUM.south(), 0.0);
    EXPECT_NEAR(M_PI, Rectangle::MAXIMUM.east(), 0.0);
    EXPECT_NEAR(M_PI * 0.5, Rectangle::MAXIMUM.north(), 0.0);
}

TEST(RectangleTest, FromDegreesConvertsAllBoundariesLikeCesiumNative) {
    // Ported from cesium-native GlobeRectangle::fromDegrees.
    const Rectangle rectangle = Rectangle::fromDegrees(0.0, 20.0, 10.0, 30.0);

    EXPECT_DOUBLE_EQ(0.0, rectangle.west());
    EXPECT_DOUBLE_EQ(20.0 * kDegToRad, rectangle.south());
    EXPECT_DOUBLE_EQ(10.0 * kDegToRad, rectangle.east());
    EXPECT_DOUBLE_EQ(30.0 * kDegToRad, rectangle.north());
}

TEST(RectangleTest, ExactEqualityMatchesCesiumNativeFieldComparison) {
    // Ported from cesium-native GlobeRectangle::equals.
    const Rectangle simple(0.1, 0.2, 0.3, 0.4);

    EXPECT_EQ(simple, simple);
    EXPECT_EQ(simple, Rectangle(0.1, 0.2, 0.3, 0.4));
    EXPECT_NE(simple, Rectangle(0.11, 0.2, 0.3, 0.4));
    EXPECT_NE(simple, Rectangle(0.1, 0.202, 0.3, 0.4));
    EXPECT_NE(simple, Rectangle(0.1, 0.2, 0.300004, 0.4));
    EXPECT_NE(simple, Rectangle(0.1, 0.2, 0.3, 0.5));
}

TEST(RectangleTest, EqualsEpsilonMatchesCesiumNative) {
    // Ported from cesium-native CesiumGeospatial/test/TestGlobeRectangle.cpp.
    const Rectangle simple(0.1, 0.2, 0.3, 0.4);

    EXPECT_TRUE(simple.equalsEpsilon(simple, 1e-6));
    EXPECT_TRUE(simple.equalsEpsilon(Rectangle(0.1, 0.2, 0.3, 0.4), 1e-6));
    EXPECT_TRUE(simple.equalsEpsilon(Rectangle(0.10001, 0.2, 0.3, 0.4), 1e-3));
    EXPECT_TRUE(simple.equalsEpsilon(Rectangle(0.1, 0.2002, 0.3, 0.4), 1e-3));
    EXPECT_TRUE(simple.equalsEpsilon(Rectangle(0.1, 0.2, 0.30003, 0.4), 1e-3));
    EXPECT_TRUE(simple.equalsEpsilon(Rectangle(0.1, 0.2, 0.3, 0.4004), 1e-3));

    EXPECT_FALSE(simple.equalsEpsilon(Rectangle(0.11, 0.2, 0.3, 0.4), 1e-3));
    EXPECT_FALSE(simple.equalsEpsilon(Rectangle(0.1, 0.202, 0.3, 0.4), 1e-3));
    EXPECT_FALSE(simple.equalsEpsilon(Rectangle(0.1, 0.2, 0.301, 0.4), 1e-3));
    EXPECT_FALSE(simple.equalsEpsilon(Rectangle(0.1, 0.2, 0.3, 0.5), 1e-3));
}

TEST(RectangleTest, ContainsMatchesCesiumNativeRadianWrappingCase) {
    // Ported from cesium-native CesiumGeospatial/test/TestGlobeRectangle.cpp.
    Rectangle wrapping(3.0, 0.2, -3.1, 0.4);

    EXPECT_TRUE(wrapping.crossesAntimeridian());
    EXPECT_TRUE(wrapping.contains(M_PI, 0.2));
    EXPECT_TRUE(wrapping.contains(-M_PI, 0.2));
    EXPECT_TRUE(wrapping.contains(3.14, 0.2));
    EXPECT_TRUE(wrapping.contains(-3.14, 0.2));
    EXPECT_FALSE(wrapping.contains(0.0, 0.2));
}

TEST(RectangleTest, ContainsMatchesCesiumNativeAntimeridianCase) {
    // Ported from cesium-native CesiumGeospatial/test/TestGlobeRectangle.cpp:
    // GlobeRectangle::contains treats west > east as an antimeridian wrap.
    Rectangle wrapping = Rectangle::fromDegrees(171.88733853924697,
                                                2.412401115076969,
                                                -156.94014420411052,
                                                28.635979979406644);

    EXPECT_TRUE(wrapping.crossesAntimeridian());
    EXPECT_TRUE(wrapping.contains(171.88733853924697 * kDegToRad,
                                  2.412401115076969 * kDegToRad));
    EXPECT_TRUE(wrapping.contains(M_PI, 2.412401115076969 * kDegToRad));
    EXPECT_TRUE(wrapping.contains(-M_PI, 2.412401115076969 * kDegToRad));
    EXPECT_TRUE(wrapping.contains(-156.94014420411052 * kDegToRad,
                                  28.635979979406644 * kDegToRad));
    EXPECT_FALSE(wrapping.contains(0.0, 2.412401115076969 * kDegToRad));
}

TEST(RectangleTest, WidthMatchesCesiumNativeWrappedLongitudeSpan) {
    // Equivalent to cesium-native computeNormalizedCoordinates' wrapped
    // rectangle width: from 175E to 175W spans 10 degrees through +/-180.
    Rectangle wrapping = Rectangle::fromDegrees(175.0, 0.0, -175.0, 10.0);

    EXPECT_TRUE(wrapping.crossesAntimeridian());
    EXPECT_NEAR(10.0 * kDegToRad, wrapping.width(), 1e-14);
}

TEST(RectangleTest, WidthAndHeightMatchCesiumNativeSimpleCase) {
    // Ported from cesium-native GlobeRectangle::computeWidth/computeHeight.
    const Rectangle simple(0.1, 0.2, 0.3, 0.4);

    EXPECT_NEAR(0.2, simple.width(), 1e-14);
    EXPECT_NEAR(0.2, simple.height(), 1e-14);
}

TEST(RectangleTest, ContainsRectangleMatchesCesiumNativeFullyContains) {
    // Source-derived from cesium-native CesiumGeometry::Rectangle::fullyContains.
    // gis-md Rectangle stores geodetic radians, so use a non-wrapping range.
    const Rectangle outer(0.0, 0.0, 1.0, 1.0);

    EXPECT_TRUE(outer.contains(Rectangle(0.0, 0.0, 1.0, 1.0)));
    EXPECT_TRUE(outer.contains(Rectangle(0.2, 0.3, 0.8, 0.9)));
    EXPECT_FALSE(outer.contains(Rectangle(-0.1, 0.3, 0.8, 0.9)));
    EXPECT_FALSE(outer.contains(Rectangle(0.2, -0.1, 0.8, 0.9)));
    EXPECT_FALSE(outer.contains(Rectangle(0.2, 0.3, 1.1, 0.9)));
    EXPECT_FALSE(outer.contains(Rectangle(0.2, 0.3, 0.8, 1.1)));
}

TEST(RectangleTest, IntersectsMatchesCesiumNativeOverlaps) {
    // Source-derived from cesium-native CesiumGeometry::Rectangle::overlaps.
    // gis-md Rectangle stores geodetic radians, so use a non-wrapping range.
    const Rectangle rectangle(0.0, 0.0, 1.0, 1.0);

    EXPECT_TRUE(rectangle.intersects(Rectangle(0.5, 0.5, 1.5, 1.5)));
    EXPECT_TRUE(rectangle.intersects(Rectangle(-0.5, -0.5, 0.5, 0.5)));
    EXPECT_TRUE(rectangle.intersects(Rectangle(0.2, 0.2, 0.8, 0.8)));
    EXPECT_FALSE(rectangle.intersects(Rectangle(1.0, 0.2, 1.2, 0.8)));
    EXPECT_FALSE(rectangle.intersects(Rectangle(0.2, 1.0, 0.8, 1.2)));
    EXPECT_FALSE(rectangle.intersects(Rectangle(1.1, 0.2, 1.2, 0.8)));
    EXPECT_FALSE(rectangle.intersects(Rectangle(0.2, 1.1, 0.8, 1.2)));
}

TEST(RectangleTest, WidthMatchesCesiumNativeBigWrappedLongitudeSpan) {
    // Equivalent to cesium-native computeNormalizedCoordinates' bigWrapping
    // case: from 179E to 10E spans 191 degrees through +/-180.
    Rectangle bigWrapping = Rectangle::fromDegrees(179.0, -10.0, 10.0, 10.0);

    EXPECT_TRUE(bigWrapping.crossesAntimeridian());
    EXPECT_NEAR(191.0 * kDegToRad, bigWrapping.width(), 1e-14);
}

TEST(RectangleTest, AntimeridianNormalizedCoordinatesMatchCesiumNative) {
    // Ported from cesium-native CesiumGeospatial/test/TestGlobeRectangle.cpp:
    // GlobeRectangle::computeNormalizedCoordinates for wrapped rectangles.
    Rectangle wrapping = Rectangle::fromDegrees(175.0, 0.0, -175.0, 10.0);

    auto westMid = wrapping.normalizedCoordinates(
        175.0 * kDegToRad, 5.0 * kDegToRad);
    EXPECT_NEAR(0.0, westMid.first, 1e-14);
    EXPECT_NEAR(0.5, westMid.second, 1e-14);

    auto eastAt180 = wrapping.normalizedCoordinates(
        M_PI, 10.0 * kDegToRad);
    EXPECT_NEAR(0.5, eastAt180.first, 1e-14);
    EXPECT_NEAR(1.0, eastAt180.second, 1e-14);

    auto westAtMinus180 = wrapping.normalizedCoordinates(-M_PI, 0.0);
    EXPECT_NEAR(0.5, westAtMinus180.first, 1e-14);
    EXPECT_NEAR(0.0, westAtMinus180.second, 1e-14);

    auto threeQuarter = wrapping.normalizedCoordinates(
        -177.5 * kDegToRad, 0.0);
    EXPECT_NEAR(0.75, threeQuarter.first, 1e-14);
    EXPECT_NEAR(0.0, threeQuarter.second, 1e-14);

    auto eastEdge = wrapping.normalizedCoordinates(
        -175.0 * kDegToRad, 0.0);
    EXPECT_NEAR(1.0, eastEdge.first, 1e-14);
    EXPECT_NEAR(0.0, eastEdge.second, 1e-14);
}

TEST(RectangleTest, NormalizedCoordinatesOutsideRectangleMatchCesiumNative) {
    // Ported from cesium-native CesiumGeospatial/test/TestGlobeRectangle.cpp:
    // computeNormalizedCoordinates is linear and intentionally does not clamp.
    Rectangle tile = Rectangle::fromDegrees(0.5, 0.0, 1.0, 0.5);

    auto westOutside = tile.normalizedCoordinates(
        0.25 * kDegToRad, 0.25 * kDegToRad);
    EXPECT_NEAR(-0.5, westOutside.first, 1e-14);
    EXPECT_NEAR(0.5, westOutside.second, 1e-14);

    auto northOutside = tile.normalizedCoordinates(
        0.5 * kDegToRad, 0.75 * kDegToRad);
    EXPECT_NEAR(0.0, northOutside.first, 1e-14);
    EXPECT_NEAR(1.5, northOutside.second, 1e-14);

    auto inside = tile.normalizedCoordinates(
        0.75 * kDegToRad, 0.25 * kDegToRad);
    EXPECT_NEAR(0.5, inside.first, 1e-14);
    EXPECT_NEAR(0.5, inside.second, 1e-14);
}

TEST(RectangleTest, BigAntimeridianNormalizedCoordinatesMatchCesiumNative) {
    // Ported from cesium-native's bigWrapping case: rectangle width is
    // 191 degrees, and longitude 5E is 186 degrees east of the west edge.
    Rectangle bigWrapping = Rectangle::fromDegrees(179.0, -10.0, 10.0, 10.0);

    auto coord = bigWrapping.normalizedCoordinates(5.0 * kDegToRad, 0.0);
    EXPECT_NEAR(186.0 / 191.0, coord.first, 1e-14);
    EXPECT_NEAR(0.5, coord.second, 1e-14);
}

TEST(RectangleTest, CenterMatchesCesiumNativeWrappedCases) {
    Rectangle simple(0.1, 0.2, 0.3, 0.4);
    auto center = simple.center();
    EXPECT_NEAR(0.2, center.first, 1e-14);
    EXPECT_NEAR(0.3, center.second, 1e-14);

    Rectangle wrapping(3.0, 0.2, -3.1, 0.4);
    center = wrapping.center();
    double expectedLongitude = 3.0 + ((M_PI - 3.0) + (-3.1 - -M_PI)) * 0.5;
    EXPECT_NEAR(expectedLongitude, center.first, 1e-14);
    EXPECT_NEAR(0.3, center.second, 1e-14);

    Rectangle wrapping2(3.1, 0.2, -3.0, 0.4);
    center = wrapping2.center();
    expectedLongitude = -3.0 - ((M_PI - 3.1) + (-3.0 - -M_PI)) * 0.5;
    EXPECT_NEAR(expectedLongitude, center.first, 1e-14);
    EXPECT_NEAR(0.3, center.second, 1e-14);
}

TEST(RectangleTest, SplitAtAntimeridianMatchesCesiumNativeOrdering) {
    Rectangle nonCrossing = Rectangle::fromDegrees(-10.0, -20.0, 30.0, 40.0);
    auto split = nonCrossing.splitAtAntimeridian();
    EXPECT_EQ(nonCrossing, split.first);
    EXPECT_FALSE(split.second.has_value());

    Rectangle nonCrossing2 = Rectangle::fromDegrees(10.0, -20.0, 30.0, 40.0);
    split = nonCrossing2.splitAtAntimeridian();
    EXPECT_EQ(nonCrossing2, split.first);
    EXPECT_FALSE(split.second.has_value());

    Rectangle crossing1 = Rectangle::fromDegrees(160.0, -20.0, -170.0, 40.0);
    split = crossing1.splitAtAntimeridian();
    EXPECT_NEAR(crossing1.west(), split.first.west(), 1e-14);
    EXPECT_NEAR(M_PI, split.first.east(), 1e-14);
    ASSERT_TRUE(split.second.has_value());
    EXPECT_NEAR(-M_PI, split.second->west(), 1e-14);
    EXPECT_NEAR(crossing1.east(), split.second->east(), 1e-14);

    Rectangle crossing2 = Rectangle::fromDegrees(170.0, -20.0, -160.0, 40.0);
    split = crossing2.splitAtAntimeridian();
    EXPECT_NEAR(-M_PI, split.first.west(), 1e-14);
    EXPECT_NEAR(crossing2.east(), split.first.east(), 1e-14);
    ASSERT_TRUE(split.second.has_value());
    EXPECT_NEAR(crossing2.west(), split.second->west(), 1e-14);
    EXPECT_NEAR(M_PI, split.second->east(), 1e-14);

    Rectangle crossing3 = Rectangle::fromDegrees(-10.0, -20.0, -160.0, 40.0);
    split = crossing3.splitAtAntimeridian();
    EXPECT_NEAR(crossing3.west(), split.first.west(), 1e-14);
    EXPECT_NEAR(M_PI, split.first.east(), 1e-14);
    ASSERT_TRUE(split.second.has_value());
    EXPECT_NEAR(-M_PI, split.second->west(), 1e-14);
    EXPECT_NEAR(crossing3.east(), split.second->east(), 1e-14);
}

TEST(RectangleTest, ComputeIntersectionMatchesCesiumNativeGlobeRectangleBranches) {
    // Cesium-native GlobeRectangle::computeIntersection accounts for
    // antimeridian wrapping before applying longitude/latitude overlap.
    const Rectangle simple = Rectangle::fromDegrees(-10.0, -20.0, 30.0, 40.0);
    const Rectangle overlapping = Rectangle::fromDegrees(20.0, -10.0, 50.0, 10.0);
    const std::optional<Rectangle> simpleIntersection =
        simple.computeIntersection(overlapping);
    ASSERT_TRUE(simpleIntersection.has_value());
    EXPECT_TRUE(simpleIntersection->equalsEpsilon(
        Rectangle::fromDegrees(20.0, -10.0, 30.0, 10.0),
        1e-14));

    const Rectangle crossing = Rectangle::fromDegrees(170.0, -10.0, -170.0, 10.0);
    const Rectangle eastern = Rectangle::fromDegrees(175.0, -5.0, 178.0, 5.0);
    const std::optional<Rectangle> easternIntersection =
        crossing.computeIntersection(eastern);
    ASSERT_TRUE(easternIntersection.has_value());
    EXPECT_TRUE(easternIntersection->equalsEpsilon(
        Rectangle::fromDegrees(175.0, -5.0, 178.0, 5.0),
        1e-14));

    const Rectangle western = Rectangle::fromDegrees(-178.0, -5.0, -175.0, 5.0);
    const std::optional<Rectangle> westernIntersection =
        crossing.computeIntersection(western);
    ASSERT_TRUE(westernIntersection.has_value());
    EXPECT_TRUE(westernIntersection->equalsEpsilon(
        Rectangle::fromDegrees(-178.0, -5.0, -175.0, 5.0),
        1e-14));

    EXPECT_FALSE(simple.computeIntersection(
        Rectangle::fromDegrees(40.0, -10.0, 50.0, 10.0)).has_value());
    EXPECT_FALSE(simple.computeIntersection(
        Rectangle::fromDegrees(0.0, 40.0, 10.0, 45.0)).has_value());
}

TEST(RectangleTest, ComputeUnionMatchesCesiumNativeGlobeRectangleBranches) {
    // Cesium-native GlobeRectangle::computeUnion preserves antimeridian
    // wrapping when that is the shorter represented union.
    const Rectangle simple = Rectangle::fromDegrees(-10.0, -20.0, 30.0, 40.0);
    EXPECT_TRUE(simple.computeUnion(
                    Rectangle::fromDegrees(20.0, -10.0, 50.0, 10.0))
                    .equalsEpsilon(Rectangle::fromDegrees(-10.0, -20.0, 50.0, 40.0),
                                   1e-14));

    const Rectangle crossing = Rectangle::fromDegrees(170.0, -10.0, -170.0, 10.0);
    EXPECT_TRUE(crossing.computeUnion(
                    Rectangle::fromDegrees(175.0, -5.0, 178.0, 5.0))
                    .equalsEpsilon(crossing, 1e-14));

    EXPECT_TRUE(crossing.computeUnion(
                    Rectangle::fromDegrees(-178.0, -5.0, -160.0, 5.0))
                    .equalsEpsilon(Rectangle::fromDegrees(170.0, -10.0, -160.0, 10.0),
                                   1e-14));

    EXPECT_TRUE(Rectangle::fromDegrees(-170.0, -10.0, 170.0, 10.0)
                    .computeUnion(Rectangle::fromDegrees(175.0, -5.0, -175.0, 5.0))
                    .equalsEpsilon(Rectangle::fromDegrees(-170.0, -10.0, -175.0, 10.0),
                                   1e-14));
}

TEST(RectangleTest, ComputeUnionMatchesCesiumNativeGeometryCases) {
    // CesiumGeometry::Rectangle uses arbitrary 2D coordinates; gis-md
    // Rectangle stores geodetic radians, so this mirrors the same overlap
    // cases inside the non-wrapping longitude range.
    const Rectangle a(0.1, 0.2, 0.3, 0.4);
    const Rectangle b(0.0, 0.0, 1.0, 1.0);
    const Rectangle c(0.15, 0.25, 0.35, 0.45);
    const Rectangle d(0.05, 0.15, 0.25, 0.35);
    const Rectangle e(1.0, 1.1, 1.2, 1.3);

    EXPECT_EQ(Rectangle(0.0, 0.0, 1.0, 1.0), a.computeUnion(b));
    EXPECT_EQ(Rectangle(0.0, 0.0, 1.0, 1.0), b.computeUnion(a));

    EXPECT_EQ(Rectangle(0.1, 0.2, 0.35, 0.45), a.computeUnion(c));
    EXPECT_EQ(Rectangle(0.1, 0.2, 0.35, 0.45), c.computeUnion(a));

    EXPECT_EQ(Rectangle(0.05, 0.15, 0.3, 0.4), a.computeUnion(d));
    EXPECT_EQ(Rectangle(0.05, 0.15, 0.3, 0.4), d.computeUnion(a));

    EXPECT_EQ(Rectangle(0.1, 0.2, 1.2, 1.3), a.computeUnion(e));
    EXPECT_EQ(Rectangle(0.1, 0.2, 1.2, 1.3), e.computeUnion(a));
}

TEST(RectangleTest, ComputeSignedDistanceMatchesCesiumNative) {
    // Ported from cesium-native CesiumGeometry/test/TestRectangle.cpp:
    // inside returns negative distance to closest edge; outside returns
    // axis distance or corner distance.
    const Rectangle positive(10.0, 20.0, 30.0, 40.0);
    const Rectangle negative(-30.0, -40.0, -10.0, -20.0);
    const double cornerDistance = std::sqrt(5.0 * 5.0 + 5.0 * 5.0);

    struct Case {
        Rectangle rectangle;
        double x;
        double y;
        double expected;
    };
    const Case cases[] = {
        {positive, 20.0, 30.0, -10.0},
        {negative, -20.0, -30.0, -10.0},
        {positive, -5.0, 30.0, 15.0},
        {negative, 5.0, -30.0, 15.0},
        {positive, 45.0, 30.0, 15.0},
        {negative, -45.0, -30.0, 15.0},
        {positive, 20.0, 5.0, 15.0},
        {negative, -20.0, -5.0, 15.0},
        {positive, 20.0, 55.0, 15.0},
        {negative, -20.0, -55.0, 15.0},
        {positive, 5.0, 15.0, cornerDistance},
        {negative, -5.0, -15.0, cornerDistance},
        {positive, 5.0, 45.0, cornerDistance},
        {negative, -5.0, -45.0, cornerDistance},
        {positive, 35.0, 15.0, cornerDistance},
        {negative, -35.0, -15.0, cornerDistance},
        {positive, 35.0, 45.0, cornerDistance},
        {negative, -35.0, -45.0, cornerDistance}
    };

    for (const Case& c : cases) {
        EXPECT_NEAR(c.expected,
                    c.rectangle.computeSignedDistance(c.x, c.y),
                    1e-13);
    }
}
