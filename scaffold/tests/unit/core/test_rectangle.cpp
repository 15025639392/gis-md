#include <gtest/gtest.h>
#include <cmath>
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
