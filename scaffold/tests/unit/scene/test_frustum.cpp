#include <gtest/gtest.h>
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/Frustum.h"

using namespace earth_engine;

namespace {

Frustum makeTestFrustum() {
    Camera camera;
    camera.lookAt(Vec3(0.0, 0.0, 10.0), Vec3::zero(), Vec3::unitY());
    camera.setPerspective(3.14159265358979323846 / 2.0, 1.0, 20.0);
    return camera.frustum(800.0, 800.0);
}

} // namespace

TEST(FrustumTest, ContainsPointInFrontOfCamera) {
    Frustum frustum = makeTestFrustum();

    EXPECT_TRUE(frustum.containsPoint(Vec3(0.0, 0.0, 0.0)));
    EXPECT_TRUE(frustum.containsPoint(Vec3(0.5, 0.5, 9.0)));
}

TEST(FrustumTest, RejectsPointBehindCamera) {
    Frustum frustum = makeTestFrustum();

    EXPECT_FALSE(frustum.containsPoint(Vec3(0.0, 0.0, 12.0)));
}

TEST(FrustumTest, HandlesNearAndFarPlaneTolerance) {
    Frustum frustum = makeTestFrustum();

    EXPECT_TRUE(frustum.containsPoint(Vec3(0.0, 0.0, 9.0), 1e-9));
    EXPECT_TRUE(frustum.containsPoint(Vec3(0.0, 0.0, -10.0), 1e-9));
    EXPECT_FALSE(frustum.containsPoint(Vec3(0.0, 0.0, 9.01), 1e-9));
    EXPECT_FALSE(frustum.containsPoint(Vec3(0.0, 0.0, -10.01), 1e-9));
}

TEST(FrustumTest, IntersectsSphereConservatively) {
    Frustum frustum = makeTestFrustum();

    EXPECT_TRUE(frustum.intersectsSphere(Vec3(0.0, 0.0, 0.0), 1.0));
    EXPECT_TRUE(frustum.intersectsSphere(Vec3(0.0, 0.0, 9.5), 0.6));
    EXPECT_FALSE(frustum.intersectsSphere(Vec3(100.0, 0.0, 0.0), 1.0));
}

TEST(FrustumTest, PlanesAreNormalized) {
    Frustum frustum = makeTestFrustum();

    for (auto index : {Frustum::PlaneIndex::Left,
                       Frustum::PlaneIndex::Right,
                       Frustum::PlaneIndex::Bottom,
                       Frustum::PlaneIndex::Top,
                       Frustum::PlaneIndex::Near,
                       Frustum::PlaneIndex::Far}) {
        EXPECT_NEAR(1.0, frustum.plane(index).normal.length(), 1e-12);
    }
}

TEST(FrustumTest, ExtractedPlanesMatchCesiumNativeCullingVolumeGolden) {
    // Ported from the behavior covered by cesium-native
    // CesiumGeometry/test/TestCullingVolume.cpp. For a camera at (0,0,10)
    // looking down -Z with a 90 degree vertical FOV, near=1, far=20, these
    // are the world-space half spaces after reverse-Z matrix extraction.
    Frustum frustum = makeTestFrustum();

    auto expectPlane = [&](Frustum::PlaneIndex index,
                           const Vec3& normal,
                           double distance) {
        const FrustumPlane& plane = frustum.plane(index);
        EXPECT_NEAR(normal.x(), plane.normal.x(), 1e-12);
        EXPECT_NEAR(normal.y(), plane.normal.y(), 1e-12);
        EXPECT_NEAR(normal.z(), plane.normal.z(), 1e-12);
        EXPECT_NEAR(distance, plane.distance, 1e-12);
    };

    const double invSqrt2 = std::sqrt(0.5);
    expectPlane(Frustum::PlaneIndex::Left,
                Vec3(invSqrt2, 0.0, -invSqrt2),
                10.0 * invSqrt2);
    expectPlane(Frustum::PlaneIndex::Right,
                Vec3(-invSqrt2, 0.0, -invSqrt2),
                10.0 * invSqrt2);
    expectPlane(Frustum::PlaneIndex::Bottom,
                Vec3(0.0, invSqrt2, -invSqrt2),
                10.0 * invSqrt2);
    expectPlane(Frustum::PlaneIndex::Top,
                Vec3(0.0, -invSqrt2, -invSqrt2),
                10.0 * invSqrt2);
    expectPlane(Frustum::PlaneIndex::Near, Vec3(0.0, 0.0, -1.0), 9.0);
    expectPlane(Frustum::PlaneIndex::Far, Vec3(0.0, 0.0, 1.0), 10.0);
}

TEST(FrustumTest, ComputeVisibilityDistinguishesInsideIntersectingOutside) {
    Frustum frustum = makeTestFrustum();

    EXPECT_EQ(CullingResult::Inside,
              frustum.computeVisibility(BoundingSphere(Vec3::zero(), 0.25)));
    EXPECT_EQ(CullingResult::Intersecting,
              frustum.computeVisibility(BoundingSphere(Vec3(0.0, 0.0, 9.5), 0.75)));
    EXPECT_EQ(CullingResult::Outside,
              frustum.computeVisibility(BoundingSphere(Vec3(0.0, 0.0, 11.0), 0.25)));
}
