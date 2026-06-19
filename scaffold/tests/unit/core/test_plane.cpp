#include <gtest/gtest.h>
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Vec3.h"
#include <stdexcept>

using namespace earth_engine;

TEST(PlaneTest, ConstructorRequiresNormalizedNormal) {
    // Ported from cesium-native CesiumGeometry/test/TestPlane.cpp.
    EXPECT_NO_THROW(Plane(Vec3::unitX(), 0.0));
    EXPECT_NO_THROW(Plane(Vec3(1.0 + 1e-6, 0.0, 0.0), 0.0));
    EXPECT_THROW(Plane(Vec3(1.0, 2.0, 3.0), 0.0), std::invalid_argument);
    EXPECT_THROW(Plane(Vec3(1.0 + 2e-6, 0.0, 0.0), 0.0),
                 std::invalid_argument);
    EXPECT_THROW(Plane(Vec3::zero(), Vec3(1.0, 2.0, 3.0)),
                 std::invalid_argument);
}

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

TEST(PlaneTest, ProjectPointOntoPlaneMatchesCesiumNative) {
    Plane yzPlane(Vec3::unitX(), 0.0);
    EXPECT_EQ(Vec3(0.0, 1.0, 0.0),
              yzPlane.projectPointOntoPlane(Vec3(1.0, 1.0, 0.0)));

    Plane zxPlane(Vec3::unitY(), 0.0);
    EXPECT_EQ(Vec3(1.0, 0.0, 0.0),
              zxPlane.projectPointOntoPlane(Vec3(1.0, 1.0, 0.0)));
}

TEST(PlaneTest, OriginPlaneConstantsMatchCesiumNativeAxes) {
    EXPECT_EQ(Vec3::unitZ(), Plane::ORIGIN_XY.getNormal());
    EXPECT_DOUBLE_EQ(0.0, Plane::ORIGIN_XY.getDistance());

    EXPECT_EQ(Vec3::unitX(), Plane::ORIGIN_YZ.getNormal());
    EXPECT_DOUBLE_EQ(0.0, Plane::ORIGIN_YZ.getDistance());

    EXPECT_EQ(Vec3::unitY(), Plane::ORIGIN_ZX.getNormal());
    EXPECT_DOUBLE_EQ(0.0, Plane::ORIGIN_ZX.getDistance());
}
