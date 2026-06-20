#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>
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

std::optional<Plane> transformedPlane(const Vec3& center,
                                      const Vec3& axis0,
                                      const Vec3& axis1,
                                      const Vec3& axis2,
                                      const Vec3& localNormal,
                                      double localDistance) {
    glm::dmat3 axes(axis0.raw(), axis1.raw(), axis2.raw());
    glm::dvec3 n = localNormal.raw();
    const glm::dvec3 arbitrary(357.0, 924.0, 258.0);
    glm::dvec3 p0 = glm::normalize(n) * -localDistance;
    glm::dvec3 tangent = glm::normalize(glm::cross(n, arbitrary));
    glm::dvec3 binormal = glm::normalize(glm::cross(n, tangent));

    p0 = axes * p0;
    tangent = axes * tangent;
    binormal = axes * binormal;

    glm::dvec3 worldNormal = glm::cross(tangent, binormal);
    if (glm::length(worldNormal) == 0.0) {
        return std::nullopt;
    }
    worldNormal = glm::normalize(worldNormal);

    const glm::dvec3 worldPoint = p0 + center.raw();
    const double distance = -glm::dot(worldPoint, worldNormal);
    if (std::abs(distance) <= 0.0001 ||
        glm::dot(worldNormal, worldNormal) <= 0.0001) {
        return std::nullopt;
    }
    return Plane(Vec3(worldNormal), distance);
}

void expectTransformedPlaneClassifications(const Vec3& center,
                                           const Vec3& axis0,
                                           const Vec3& axis1,
                                           const Vec3& axis2) {
    OrientedBoundingBox box(center, axis0 * 0.5, axis1 * 0.5, axis2 * 0.5);
    const double edgeDistance = std::sqrt(0.5);

    int checkedPlanes = 0;
    auto expectIfPlane = [&](const std::optional<Plane>& plane,
                             int expected) {
        if (!plane.has_value()) return;
        ++checkedPlanes;
        EXPECT_EQ(expected, box.intersectPlane(*plane));
    };

    std::optional<Plane> plane = transformedPlane(
        center, axis0, axis1, axis2, Vec3(1.0, 0.0, 0.0), 0.50001);
    expectIfPlane(plane, 1);

    plane = transformedPlane(
        center, axis0, axis1, axis2, Vec3(1.0, 0.0, 0.0), 0.49999);
    expectIfPlane(plane, 0);

    plane = transformedPlane(
        center, axis0, axis1, axis2, Vec3(1.0, 0.0, 0.0), -0.50001);
    expectIfPlane(plane, -1);

    plane = transformedPlane(
        center,
        axis0,
        axis1,
        axis2,
        Vec3(1.0, 1.0, 0.0),
        edgeDistance + 0.00001);
    expectIfPlane(plane, 1);

    plane = transformedPlane(
        center,
        axis0,
        axis1,
        axis2,
        Vec3(1.0, 1.0, 0.0),
        -edgeDistance - 0.00001);
    expectIfPlane(plane, -1);

    EXPECT_GT(checkedPlanes, 0);
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

TEST(OrientedBoundingBoxTest, IntersectPlaneTangentBoundariesMatchCesiumNative) {
    // cesium-native classifies distance <= -effectiveRadius as Outside and
    // distance >= effectiveRadius as Inside.
    OrientedBoundingBox box = unitBox();

    EXPECT_EQ(1, box.intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), 0.5)));
    EXPECT_EQ(0, box.intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), 0.0)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(Vec3(1.0, 0.0, 0.0), -0.5)));
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

TEST(OrientedBoundingBoxTest, IntersectPlaneMatchesCesiumNativeTransformedCases) {
    // Ported from cesium-native
    // CesiumGeometry/test/TestOrientedBoundingBox.cpp transformed cases.
    expectTransformedPlaneClassifications(
        Vec3(1.0, 0.0, 0.0),
        Vec3::unitX(),
        Vec3::unitY(),
        Vec3::unitZ());

    const Mat4 rotation =
        Mat4::rotationZ(1.2) * Mat4::rotationY(0.5) * Mat4::rotationX(-1.2);
    expectTransformedPlaneClassifications(
        Vec3::zero(),
        rotation.transformVector(Vec3::unitX()),
        rotation.transformVector(Vec3::unitY()),
        rotation.transformVector(Vec3::unitZ()));

    expectTransformedPlaneClassifications(
        Vec3(-5.1, 0.0, 0.1),
        rotation.transformVector(Vec3(1.5, 0.0, 0.0)),
        rotation.transformVector(Vec3(0.0, 80.4, 0.0)),
        rotation.transformVector(Vec3(0.0, 0.0, 2.6)));
}

TEST(OrientedBoundingBoxTest, IntersectPlaneSkipsSingularPlaneLikeCesiumNative) {
    // Cesium-native's transformed-plane test treats singular test planes as
    // absent; the box intersection itself remains defined for zero half axes.
    const Vec3 center = Vec3::zero();
    const std::optional<Plane> singularPlane = transformedPlane(
        center,
        Vec3::zero(),
        Vec3(0.0, 0.4, 0.0),
        Vec3(0.0, 0.0, 20.6),
        Vec3(1.0, 0.0, 0.0),
        0.50001);
    EXPECT_FALSE(singularPlane.has_value());

    OrientedBoundingBox box(center,
                            Vec3::zero(),
                            Vec3(0.0, 0.2, 0.0),
                            Vec3(0.0, 0.0, 10.3));
    EXPECT_EQ(1, box.intersectPlane(Plane(Vec3::unitX(), 0.00001)));
    EXPECT_EQ(-1, box.intersectPlane(Plane(Vec3::unitX(), -0.00001)));
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

TEST(OrientedBoundingBoxTest, DistanceSquaredSortsBackToFrontLikeCesiumNativeExample) {
    const Vec3 cameraPosition = Vec3::zero();
    std::vector<OrientedBoundingBox> boxes{
        OrientedBoundingBox(Vec3(1.0, 0.0, 0.0),
                            Vec3(0.5, 0.0, 0.0),
                            Vec3(0.0, 0.5, 0.0),
                            Vec3(0.0, 0.0, 0.5)),
        OrientedBoundingBox(Vec3(2.0, 0.0, 0.0),
                            Vec3(0.5, 0.0, 0.0),
                            Vec3(0.0, 0.5, 0.0),
                            Vec3(0.0, 0.0, 0.5))
    };

    std::sort(boxes.begin(),
              boxes.end(),
              [&cameraPosition](const OrientedBoundingBox& a,
                                const OrientedBoundingBox& b) {
                  return a.computeDistanceSquaredToPosition(cameraPosition) >
                         b.computeDistanceSquaredToPosition(cameraPosition);
              });

    EXPECT_DOUBLE_EQ(2.0, boxes[0].getCenter().x());
    EXPECT_DOUBLE_EQ(1.0, boxes[1].getCenter().x());
}

TEST(OrientedBoundingBoxTest, DistanceSquaredToPositionHandlesDegenerateAxesLikeCesiumNative) {
    // Ported from cesium-native
    // CesiumGeometry/test/TestOrientedBoundingBox.cpp degenerate-axes cases.
    const Vec3 cameraPosition = Vec3::zero();

    struct Case {
        OrientedBoundingBox box;
        double expected;
    };

    const Case cases[] = {
        {
            OrientedBoundingBox(Vec3(1.0, 0.0, 0.0),
                                Vec3::zero(),
                                Vec3::zero(),
                                Vec3::zero()),
            1.0
        },
        {
            OrientedBoundingBox(Vec3(1.0, 0.0, 0.0),
                                Vec3(1.0, 0.0, 0.0),
                                Vec3::zero(),
                                Vec3::zero()),
            0.0
        },
        {
            OrientedBoundingBox(Vec3(1.0, 0.0, 0.0),
                                Vec3(1.0, 0.0, 0.0),
                                Vec3(0.0, 1.0, 0.0),
                                Vec3::zero()),
            0.0
        },
        {
            OrientedBoundingBox(Vec3(1.0, 0.0, 0.0),
                                Vec3::zero(),
                                Vec3(0.0, 1.0, 0.0),
                                Vec3(0.0, 0.0, 1.0)),
            1.0
        }
    };

    for (const Case& testCase : cases) {
        EXPECT_DOUBLE_EQ(
            testCase.expected,
            testCase.box.computeDistanceSquaredToPosition(cameraPosition));
    }
}

TEST(OrientedBoundingBoxTest, ContainsMatchesCesiumNativeLocalUnitCube) {
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            Vec3(2.0, 0.0, 0.0),
                            Vec3(0.0, 3.0, 0.0),
                            Vec3(0.0, 0.0, 4.0));

    EXPECT_TRUE(box.contains(Vec3(1.0, 2.0, 3.0)));
    EXPECT_TRUE(box.contains(Vec3(3.0, 5.0, 7.0)));
    EXPECT_FALSE(box.contains(Vec3(3.0 + 1e-12, 5.0, 7.0)));
    EXPECT_FALSE(box.contains(Vec3(4.0, 2.0, 3.0)));
    EXPECT_FALSE(box.contains(Vec3(1.0, 6.0, 3.0)));
    EXPECT_FALSE(box.contains(Vec3(1.0, 2.0, 8.0)));
}

TEST(OrientedBoundingBoxTest, ContainsRejectsAnyPointBeyondCesiumNativeUnitBoundary) {
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            Vec3(2.0, 0.0, 0.0),
                            Vec3(0.0, 3.0, 0.0),
                            Vec3(0.0, 0.0, 4.0));

    EXPECT_TRUE(box.contains(Vec3(3.0, 5.0, 7.0)));
    EXPECT_FALSE(box.contains(Vec3(3.0 + 1e-15, 5.0, 7.0)));
}

TEST(OrientedBoundingBoxTest, InverseHalfAxesMatchesCesiumNative) {
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            Vec3(2.0, 0.0, 0.0),
                            Vec3(0.0, 4.0, 0.0),
                            Vec3(0.0, 0.0, 8.0));

    const glm::dmat3& inverseHalfAxes = box.getInverseHalfAxes();

    EXPECT_DOUBLE_EQ(0.5, inverseHalfAxes[0][0]);
    EXPECT_DOUBLE_EQ(0.25, inverseHalfAxes[1][1]);
    EXPECT_DOUBLE_EQ(0.125, inverseHalfAxes[2][2]);
    EXPECT_DOUBLE_EQ(1.0, (inverseHalfAxes * Vec3(2.0, 0.0, 0.0).raw()).x);
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

TEST(OrientedBoundingBoxTest, ToSphereIdentityHalfAxesMatchesCesiumNative) {
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            Vec3(1.0, 0.0, 0.0),
                            Vec3(0.0, 1.0, 0.0),
                            Vec3(0.0, 0.0, 1.0));

    const BoundingSphere sphere = box.toSphere();

    EXPECT_EQ(Vec3(1.0, 2.0, 3.0), sphere.getCenter());
    EXPECT_DOUBLE_EQ(std::sqrt(3.0), sphere.getRadius());
}

TEST(OrientedBoundingBoxTest, ToSphereScaledHalfAxesMatchesCesiumNative) {
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            Vec3(10.0, 0.0, 0.0),
                            Vec3(0.0, 20.0, 0.0),
                            Vec3(0.0, 0.0, 30.0));

    const BoundingSphere sphere = box.toSphere();

    EXPECT_EQ(Vec3(1.0, 2.0, 3.0), sphere.getCenter());
    EXPECT_DOUBLE_EQ(std::sqrt(10.0 * 10.0 + 20.0 * 20.0 + 30.0 * 30.0),
                     sphere.getRadius());
}

TEST(OrientedBoundingBoxTest, ToSphereRotationKeepsCesiumNativeRadius) {
    const double fortyFiveDegrees = std::acos(-1.0) / 4.0;
    const Mat4 rotation = Mat4::rotationY(fortyFiveDegrees);
    OrientedBoundingBox box(Vec3(1.0, 2.0, 3.0),
                            rotation.transformVector(Vec3(1.0, 0.0, 0.0)),
                            rotation.transformVector(Vec3(0.0, 1.0, 0.0)),
                            rotation.transformVector(Vec3(0.0, 0.0, 1.0)));

    const BoundingSphere sphere = box.toSphere();

    EXPECT_EQ(Vec3(1.0, 2.0, 3.0), sphere.getCenter());
    EXPECT_NEAR(std::sqrt(3.0), sphere.getRadius(), 1e-14);
}
