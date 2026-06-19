#include <gtest/gtest.h>
#include "earth_engine/core/math/BoundingSphere.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Vec3.h"

#include <cmath>

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

TEST(BoundingSphereTest, TransformTranslationMatchesCesiumNative) {
    BoundingSphere sphere(Vec3(1.0, 2.0, 3.0), 45.0);

    BoundingSphere transformed =
        sphere.transform(Mat4::translation(Vec3(10.0, 20.0, 30.0)));

    EXPECT_DOUBLE_EQ(45.0, transformed.getRadius());
    EXPECT_DOUBLE_EQ(11.0, transformed.getCenter().x());
    EXPECT_DOUBLE_EQ(22.0, transformed.getCenter().y());
    EXPECT_DOUBLE_EQ(33.0, transformed.getCenter().z());
}

TEST(BoundingSphereTest, TransformRotationMatchesCesiumNative) {
    BoundingSphere sphere(Vec3(1.0, 2.0, 3.0), 45.0);
    const double fortyFiveDegrees = std::acos(-1.0) / 4.0;
    const Mat4 transform = Mat4::rotationY(fortyFiveDegrees);

    BoundingSphere transformed = sphere.transform(transform);
    Vec3 rotatedCenter = transform * sphere.getCenter();

    EXPECT_DOUBLE_EQ(45.0, transformed.getRadius());
    EXPECT_NEAR(rotatedCenter.x(), transformed.getCenter().x(), 1e-14);
    EXPECT_NEAR(rotatedCenter.y(), transformed.getCenter().y(), 1e-14);
    EXPECT_NEAR(rotatedCenter.z(), transformed.getCenter().z(), 1e-14);
}

TEST(BoundingSphereTest, TransformNonUniformScaleUsesMaximumAxis) {
    BoundingSphere sphere(Vec3(1.0, 2.0, 3.0), 45.0);
    const Mat4 transform = Mat4::scale(Vec3(2.0, 3.0, 4.0));

    BoundingSphere transformed = sphere.transform(transform);

    EXPECT_DOUBLE_EQ(2.0, transformed.getCenter().x());
    EXPECT_DOUBLE_EQ(6.0, transformed.getCenter().y());
    EXPECT_DOUBLE_EQ(12.0, transformed.getCenter().z());
    EXPECT_DOUBLE_EQ(180.0, transformed.getRadius());
}

TEST(BoundingSphereTest, TransformNegativeScaleUsesAxisLengthLikeCesiumNative) {
    // Cesium-native derives the radius scale from the transformed axis lengths,
    // so mirrored axes still contribute positive scale magnitudes.
    BoundingSphere sphere(Vec3(1.0, -2.0, 3.0), 10.0);
    const Mat4 transform = Mat4::scale(Vec3(-2.0, 3.0, -4.0));

    BoundingSphere transformed = sphere.transform(transform);

    EXPECT_DOUBLE_EQ(-2.0, transformed.getCenter().x());
    EXPECT_DOUBLE_EQ(-6.0, transformed.getCenter().y());
    EXPECT_DOUBLE_EQ(-12.0, transformed.getCenter().z());
    EXPECT_DOUBLE_EQ(40.0, transformed.getRadius());
}
