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
