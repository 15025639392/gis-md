#include <gtest/gtest.h>
#include "earth_engine/core/math/BoundingSphere.h"
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

TEST(BoundingSphereTest, IntersectPlaneMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestBoundingSphere.cpp.
    EXPECT_EQ(1,
              BoundingSphere(Vec3::zero(), 0.5)
                  .intersectPlane(Plane(Vec3(-1.0, 0.0, 0.0), 1.0)));
    EXPECT_EQ(-1,
              BoundingSphere(Vec3::zero(), 0.5)
                  .intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), -1.0)));
    EXPECT_EQ(0,
              BoundingSphere(Vec3(1.0, 0.0, 0.0), 0.5)
                  .intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), -1.0)));
}

TEST(BoundingSphereTest, DistanceSquaredToPositionMatchesCesiumNativeOutside) {
    BoundingSphere sphere(Vec3::zero(), 1.0);
    Vec3 position(-2.0, 1.0, 0.0);

    EXPECT_NEAR(1.52786405,
                sphere.computeDistanceSquaredToPosition(position),
                1e-6);
}

TEST(BoundingSphereTest, DistanceSquaredToPositionMatchesCesiumNativeInside) {
    BoundingSphere sphere(Vec3::zero(), 1.0);

    EXPECT_DOUBLE_EQ(0.0,
                     sphere.computeDistanceSquaredToPosition(
                         Vec3(-0.5, 0.5, 0.0)));
}

TEST(BoundingSphereTest, ContainsMatchesCesiumNativeBoundaryCase) {
    Vec3 center(1.0, 2.0, 3.0);
    double radius = 45.0;
    BoundingSphere sphere(center, radius);
    double epsilon = 1e-14;

    EXPECT_TRUE(sphere.contains(center));
    EXPECT_TRUE(sphere.contains(center + Vec3(radius, 0.0, 0.0)));
    EXPECT_FALSE(sphere.contains(center + Vec3(radius + epsilon, 0.0, 0.0)));
}
