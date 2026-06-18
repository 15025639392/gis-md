#include <gtest/gtest.h>
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

TEST(PlaneTest, GetPointDistanceMatchesCesiumNativeFormula) {
    // Ported from cesium-native CesiumGeometry/test/TestPlane.cpp:
    // distance(point) = dot(normal, point) + planeDistance.
    Vec3 normal(1.0, 2.0, 3.0);
    normal = normal.normalized();
    Plane plane(normal, 12.34);
    Vec3 point(4.0, 5.0, 6.0);

    EXPECT_DOUBLE_EQ(normal.dot(point) + plane.getDistance(),
                     plane.getPointDistance(point));
}

TEST(PlaneTest, PointNormalConstructorMatchesCesiumNativeDistanceSign) {
    Vec3 point(4.0, 5.0, 6.0);
    Vec3 normal(1.0, 2.0, 3.0);
    normal = normal.normalized();

    Plane plane(point, normal);

    EXPECT_NEAR(0.0, plane.getPointDistance(point), 1e-14);
    EXPECT_DOUBLE_EQ(-normal.dot(point), plane.getDistance());
}
