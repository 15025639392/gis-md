#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/Scene.h"

using namespace earth_engine;

TEST(CameraTest, LookAtBuildsOrthonormalBasis) {
    Camera camera;
    camera.lookAt(Vec3(0.0, 0.0, 10.0), Vec3::zero(), Vec3::unitY());

    EXPECT_NEAR(0.0, camera.direction().x(), 1e-12);
    EXPECT_NEAR(0.0, camera.direction().y(), 1e-12);
    EXPECT_NEAR(-1.0, camera.direction().z(), 1e-12);
    EXPECT_NEAR(1.0, camera.right().x(), 1e-12);
    EXPECT_NEAR(0.0, camera.up().dot(camera.direction()), 1e-12);
    EXPECT_NEAR(0.0, camera.right().dot(camera.direction()), 1e-12);
}

TEST(CameraTest, CenterPickRayPointsAlongDirection) {
    Camera camera;
    camera.lookAt(Vec3(0.0, 0.0, 10.0), Vec3::zero(), Vec3::unitY());

    Ray ray = camera.getPickRay(400.0, 300.0, 800.0, 600.0);

    EXPECT_NEAR(0.0, ray.direction().x(), 1e-12);
    EXPECT_NEAR(0.0, ray.direction().y(), 1e-12);
    EXPECT_NEAR(-1.0, ray.direction().z(), 1e-12);
    EXPECT_NEAR(1.0, ray.direction().length(), 1e-12);
}

TEST(CameraTest, PickRayUsesViewportAspect) {
    Camera camera;
    camera.lookAt(Vec3(0.0, 0.0, 10.0), Vec3::zero(), Vec3::unitY());
    camera.setPerspective(M_PI / 2.0, 1.0, 1000.0);

    Ray rightEdge = camera.getPickRay(800.0, 300.0, 800.0, 600.0);
    Ray topEdge = camera.getPickRay(400.0, 0.0, 800.0, 600.0);

    EXPECT_GT(rightEdge.direction().x(), 0.0);
    EXPECT_NEAR(0.0, rightEdge.direction().y(), 1e-12);
    EXPECT_LT(rightEdge.direction().z(), 0.0);

    EXPECT_NEAR(0.0, topEdge.direction().x(), 1e-12);
    EXPECT_GT(topEdge.direction().y(), 0.0);
    EXPECT_LT(topEdge.direction().z(), 0.0);
    EXPECT_GT(std::abs(rightEdge.direction().x()), std::abs(topEdge.direction().y()));
}

TEST(CameraTest, RejectsInvalidPerspective) {
    Camera camera;

    EXPECT_THROW(camera.setPerspective(0.0, 1.0, 10.0), std::invalid_argument);
    EXPECT_THROW(camera.setPerspective(M_PI / 4.0, 0.0, 10.0), std::invalid_argument);
    EXPECT_THROW(camera.setPerspective(M_PI / 4.0, 10.0, 1.0), std::invalid_argument);
}

TEST(CameraTest, RejectsInvalidViewport) {
    Camera camera;

    EXPECT_THROW(camera.projectionMatrix(0.0, 600.0), std::invalid_argument);
    EXPECT_THROW(camera.getPickRay(0.0, 0.0, 800.0, 0.0), std::invalid_argument);
}

TEST(CameraTest, BuildsFrustumFromCurrentViewProjection) {
    Camera camera;
    camera.lookAt(Vec3(0.0, 0.0, 10.0), Vec3::zero(), Vec3::unitY());
    camera.setPerspective(M_PI / 2.0, 1.0, 20.0);

    Frustum frustum = camera.frustum(800.0, 800.0);

    EXPECT_TRUE(frustum.containsPoint(Vec3::zero()));
    EXPECT_FALSE(frustum.containsPoint(Vec3(0.0, 0.0, 12.0)));
}

TEST(SceneTest, DefaultCameraNearPlaneAllowsNearGroundViews) {
    Scene scene;

    EXPECT_DOUBLE_EQ(1.0, scene.camera().nearPlaneMeters());
}
