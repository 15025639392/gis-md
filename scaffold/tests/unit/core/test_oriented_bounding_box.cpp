#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/core/math/AxisAlignedBox.h"
#include "earth_engine/core/math/OrientedBoundingBox.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

namespace {
OrientedBoundingBox unitBox() {
    return OrientedBoundingBox(Vec3::zero(),
                               Vec3(0.5, 0.0, 0.0),
                               Vec3(0.0, 0.5, 0.0),
                               Vec3(0.0, 0.0, 0.5));
}
} // namespace

TEST(OrientedBoundingBoxTest, IntersectPlaneMatchesCesiumNativeFaceCases) {
    // Condensed from cesium-native
    // CesiumGeometry/test/TestOrientedBoundingBox.cpp face-plane cases.
    OrientedBoundingBox box = unitBox();

    EXPECT_EQ(1, box.intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), 0.50001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), -0.49999)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), -0.50001)));

    EXPECT_EQ(1, box.intersectPlane(Plane(Vec3(0.0, -1.0, 0.0), 0.50001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(Vec3(0.0, -1.0, 0.0), -0.49999)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(Vec3(0.0, -1.0, 0.0), -0.50001)));
}

TEST(OrientedBoundingBoxTest, IntersectPlaneMatchesCesiumNativeEdgeCase) {
    OrientedBoundingBox box = unitBox();
    const double edgeDistance = std::sqrt(0.5);
    Vec3 normal = Vec3(1.0, 1.0, 0.0).normalized();

    EXPECT_EQ(1, box.intersectPlane(Plane(normal, edgeDistance + 0.00001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(normal, edgeDistance - 0.00001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(normal, -edgeDistance + 0.00001)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(normal, -edgeDistance - 0.00001)));
}

TEST(OrientedBoundingBoxTest, IntersectPlaneMatchesCesiumNativeCornerCase) {
    OrientedBoundingBox box = unitBox();
    const double cornerDistance = std::sqrt(3.0 / 4.0);
    Vec3 normal = Vec3(1.0, 1.0, 1.0).normalized();

    EXPECT_EQ(1, box.intersectPlane(Plane(normal, cornerDistance + 0.00001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(normal, cornerDistance - 0.00001)));
    EXPECT_EQ(0, box.intersectPlane(Plane(normal, -cornerDistance + 0.00001)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(normal, -cornerDistance - 0.00001)));
}

TEST(OrientedBoundingBoxTest, DistanceSquaredToPositionClampsToBox) {
    OrientedBoundingBox box = unitBox();

    EXPECT_DOUBLE_EQ(0.0, box.computeDistanceSquaredToPosition(Vec3::zero()));
    EXPECT_DOUBLE_EQ(0.0,
                     box.computeDistanceSquaredToPosition(Vec3(0.5, 0.0, 0.0)));
    EXPECT_NEAR(2.25,
                box.computeDistanceSquaredToPosition(Vec3(2.0, 0.0, 0.0)),
                1e-14);
    EXPECT_NEAR(0.75,
                box.computeDistanceSquaredToPosition(Vec3(1.0, 1.0, 1.0)),
                1e-14);
}

TEST(OrientedBoundingBoxTest, ContainsMatchesCesiumNativeLocalUnitCube) {
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            Vec3(2.0, 0.0, 0.0),
                            Vec3(0.0, 3.0, 0.0),
                            Vec3(0.0, 0.0, 4.0));

    EXPECT_TRUE(box.contains(Vec3(1.0, 2.0, 3.0)));
    EXPECT_TRUE(box.contains(Vec3(3.0, 5.0, 7.0)));
    EXPECT_FALSE(box.contains(Vec3(3.0 + 1e-12, 5.0, 7.0)));
}

TEST(OrientedBoundingBoxTest, ContainsMatchesCesiumNativeRotatedBox) {
    const Vec3 center(10.0, 20.0, 30.0);
    const Mat4 rotation = Mat4::rotationY(std::acos(-1.0) / 4.0);
    const Vec3 axis0 = rotation.transformVector(Vec3(2.0, 0.0, 0.0));
    const Vec3 axis1 = rotation.transformVector(Vec3(0.0, 3.0, 0.0));
    const Vec3 axis2 = rotation.transformVector(Vec3(0.0, 0.0, 4.0));
    OrientedBoundingBox box(center, axis0, axis1, axis2);

    EXPECT_FALSE(box.contains(Vec3::zero()));
    EXPECT_TRUE(box.contains(center));
    EXPECT_TRUE(box.contains(center + axis0 + axis1 + axis2));
    EXPECT_TRUE(box.contains(center - axis0 - axis1 - axis2));
    EXPECT_FALSE(box.contains(center + rotation.transformVector(Vec3(3.0, 0.0, 0.0))));
    EXPECT_FALSE(box.contains(center + rotation.transformVector(Vec3(0.0, 4.0, 0.0))));
    EXPECT_FALSE(box.contains(center + rotation.transformVector(Vec3(0.0, 0.0, 5.0))));
}

TEST(OrientedBoundingBoxTest, TransformMatchesCesiumNative) {
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            Vec3(2.0, 0.0, 0.0),
                            Vec3(0.0, 3.0, 0.0),
                            Vec3(0.0, 0.0, 4.0));
    const Mat4 transform =
        Mat4::translation(Vec3(10.0, 20.0, 30.0)) *
        Mat4::scale(Vec3(2.0, 3.0, 4.0));

    OrientedBoundingBox transformed = box.transform(transform);

    EXPECT_DOUBLE_EQ(12.0, transformed.getCenter().x());
    EXPECT_DOUBLE_EQ(26.0, transformed.getCenter().y());
    EXPECT_DOUBLE_EQ(42.0, transformed.getCenter().z());
    EXPECT_DOUBLE_EQ(4.0, transformed.getHalfAxis(0).x());
    EXPECT_DOUBLE_EQ(0.0, transformed.getHalfAxis(0).y());
    EXPECT_DOUBLE_EQ(0.0, transformed.getHalfAxis(0).z());
    EXPECT_DOUBLE_EQ(0.0, transformed.getHalfAxis(1).x());
    EXPECT_DOUBLE_EQ(9.0, transformed.getHalfAxis(1).y());
    EXPECT_DOUBLE_EQ(0.0, transformed.getHalfAxis(1).z());
    EXPECT_DOUBLE_EQ(0.0, transformed.getHalfAxis(2).x());
    EXPECT_DOUBLE_EQ(0.0, transformed.getHalfAxis(2).y());
    EXPECT_DOUBLE_EQ(16.0, transformed.getHalfAxis(2).z());
}

TEST(OrientedBoundingBoxTest, ToAxisAlignedMatchesCesiumNativeAxisAlignedBox) {
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            Vec3(10.0, 0.0, 0.0),
                            Vec3(0.0, 20.0, 0.0),
                            Vec3(0.0, 0.0, 30.0));

    AxisAlignedBox aabb = box.toAxisAligned();

    EXPECT_DOUBLE_EQ(-9.0, aabb.minimumX());
    EXPECT_DOUBLE_EQ(11.0, aabb.maximumX());
    EXPECT_DOUBLE_EQ(-18.0, aabb.minimumY());
    EXPECT_DOUBLE_EQ(22.0, aabb.maximumY());
    EXPECT_DOUBLE_EQ(-27.0, aabb.minimumZ());
    EXPECT_DOUBLE_EQ(33.0, aabb.maximumZ());
}

TEST(OrientedBoundingBoxTest, ToAxisAlignedMatchesCesiumNativeRotatedBox) {
    const double fortyFiveDegrees = std::acos(-1.0) / 4.0;
    const Mat4 rotation = Mat4::rotationY(fortyFiveDegrees);
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            rotation.transformVector(Vec3(1.0, 0.0, 0.0)),
                            rotation.transformVector(Vec3(0.0, 1.0, 0.0)),
                            rotation.transformVector(Vec3(0.0, 0.0, 1.0)));

    AxisAlignedBox aabb = box.toAxisAligned();

    const double sqrt2 = std::sqrt(2.0);
    EXPECT_NEAR(1.0 - sqrt2, aabb.minimumX(), 1e-14);
    EXPECT_NEAR(1.0 + sqrt2, aabb.maximumX(), 1e-14);
    EXPECT_NEAR(2.0, aabb.lengthY(), 1e-14);
    EXPECT_NEAR(2.0 - 1.0, aabb.minimumY(), 1e-14);
    EXPECT_NEAR(2.0 + 1.0, aabb.maximumY(), 1e-14);
    EXPECT_NEAR(3.0 - sqrt2, aabb.minimumZ(), 1e-14);
    EXPECT_NEAR(3.0 + sqrt2, aabb.maximumZ(), 1e-14);
}

TEST(OrientedBoundingBoxTest, FromAxisAlignedMatchesCesiumNative) {
    const AxisAlignedBox aabb(-1.0, -2.0, -3.0, 5.0, 8.0, 11.0);

    const OrientedBoundingBox box = OrientedBoundingBox::fromAxisAligned(aabb);

    EXPECT_EQ(aabb.center(), box.getCenter());
    EXPECT_EQ(Vec3(3.0, 0.0, 0.0), box.getHalfAxis(0));
    EXPECT_EQ(Vec3(0.0, 5.0, 0.0), box.getHalfAxis(1));
    EXPECT_EQ(Vec3(0.0, 0.0, 7.0), box.getHalfAxis(2));
    EXPECT_EQ(Vec3(6.0, 10.0, 14.0), box.getLengths());
    EXPECT_TRUE(box.contains(Vec3(-1.0, -2.0, -3.0)));
    EXPECT_TRUE(box.contains(Vec3(5.0, 8.0, 11.0)));
    EXPECT_FALSE(box.contains(Vec3(5.0 + 1e-12, 8.0, 11.0)));
}

TEST(OrientedBoundingBoxTest, FromSphereBuildsCircumscribedBox) {
    BoundingSphere sphere(Vec3(1.0, 2.0, 3.0), 10.0);

    OrientedBoundingBox box = OrientedBoundingBox::fromSphere(sphere);

    EXPECT_EQ(Vec3(1.0, 2.0, 3.0), box.getCenter());
    EXPECT_EQ(Vec3(20.0, 20.0, 20.0), box.getLengths());
    EXPECT_TRUE(box.contains(Vec3(11.0, 2.0, 3.0)));
    EXPECT_FALSE(box.contains(Vec3(11.0 + 1e-12, 2.0, 3.0)));
}

TEST(OrientedBoundingBoxTest, ToSphereMatchesCesiumNativeCornerRadius) {
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            Vec3(2.0, 0.0, 0.0),
                            Vec3(0.0, 3.0, 0.0),
                            Vec3(0.0, 0.0, 6.0));

    const BoundingSphere sphere = box.toSphere();

    EXPECT_EQ(Vec3(1.0, 2.0, 3.0), sphere.getCenter());
    EXPECT_DOUBLE_EQ(7.0, sphere.getRadius());
}
