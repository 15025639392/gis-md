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
