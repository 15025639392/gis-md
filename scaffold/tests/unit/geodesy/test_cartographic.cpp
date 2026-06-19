#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"

#include <cmath>

using namespace earth_engine;

namespace {
constexpr double kDegToRad = M_PI / 180.0;
}

TEST(CartographicTest, ConstructorStoresRadiansAndDefaultHeight) {
    // Aligned with cesium-native CesiumGeospatial::Cartographic constructor:
    // longitude/latitude inputs are radians and height defaults to meters 0.
    const Cartographic cartographic(1.0, 0.5);

    EXPECT_DOUBLE_EQ(1.0, cartographic.longitude());
    EXPECT_DOUBLE_EQ(0.5, cartographic.latitude());
    EXPECT_DOUBLE_EQ(0.0, cartographic.height());
}

TEST(CartographicTest, ConstructorStoresExplicitHeightMeters) {
    const Cartographic cartographic(1.0, 0.5, 1234.5);

    EXPECT_DOUBLE_EQ(1.0, cartographic.longitude());
    EXPECT_DOUBLE_EQ(0.5, cartographic.latitude());
    EXPECT_DOUBLE_EQ(1234.5, cartographic.height());
}

TEST(CartographicTest, FromDegreesConvertsAnglesAndPreservesHeight) {
    const Cartographic cartographic =
        Cartographic::fromDegrees(180.0, -45.0, 987.0);

    EXPECT_DOUBLE_EQ(180.0 * kDegToRad, cartographic.longitude());
    EXPECT_DOUBLE_EQ(-45.0 * kDegToRad, cartographic.latitude());
    EXPECT_DOUBLE_EQ(987.0, cartographic.height());
}

TEST(CartographicTest, EqualityMatchesCesiumNativeExactFieldComparison) {
    const Cartographic a(1.0, 0.5, 100.0);
    const Cartographic same(1.0, 0.5, 100.0);
    const Cartographic differentLongitude(1.0 + 1e-15, 0.5, 100.0);
    const Cartographic differentLatitude(1.0, 0.5 + 1e-15, 100.0);
    const Cartographic differentHeight(1.0, 0.5, 100.0 + 1e-12);

    EXPECT_TRUE(a == same);
    EXPECT_FALSE(a == differentLongitude);
    EXPECT_FALSE(a == differentLatitude);
    EXPECT_FALSE(a == differentHeight);
}
