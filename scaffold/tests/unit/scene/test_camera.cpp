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

// dir ∥ up 的退化输入不允许产出 NaN 基：setOrientation 需换轴重建，
// 输出仍为正交归一（覆盖 Facade 侧曾以调用点补丁防御的整类 bug）。
TEST(CameraTest, CameraSetViewDegenerateBasisStaysOrthonormal) {
    const Vec3 parallelCases[] = {
        Vec3::unitZ(),            // up 与 direction 完全同向
        -Vec3::unitZ(),           // 完全反向
        Vec3(0.0, 0.0, 3.0),      // 同向不同长
    };
    for (const Vec3& up : parallelCases) {
        Camera camera;
        camera.setView(Vec3(0.0, 0.0, 10.0), Vec3(0.0, 0.0, -1.0), up);

        const Vec3 d = camera.direction();
        const Vec3 r = camera.right();
        const Vec3 u = camera.up();
        for (double c : {d.x(), d.y(), d.z(), r.x(), r.y(), r.z(),
                         u.x(), u.y(), u.z()}) {
            EXPECT_TRUE(std::isfinite(c));
        }
        EXPECT_NEAR(1.0, d.length(), 1e-12);
        EXPECT_NEAR(1.0, r.length(), 1e-12);
        EXPECT_NEAR(1.0, u.length(), 1e-12);
        EXPECT_NEAR(0.0, d.dot(r), 1e-12);
        EXPECT_NEAR(0.0, d.dot(u), 1e-12);
        EXPECT_NEAR(0.0, r.dot(u), 1e-12);
    }

    // 良态输入行为不变。
    Camera camera;
    camera.setView(Vec3(0.0, 0.0, 10.0), Vec3(0.0, 0.0, -1.0), Vec3::unitY());
    EXPECT_NEAR(1.0, camera.up().y(), 1e-12);
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

TEST(SceneTest, DefaultCameraUsesOpenGlobusNearPlane) {
    Scene scene;

    // OpenGlobus PlanetCamera reverse-Z: near=150, far=1e12.
    EXPECT_DOUBLE_EQ(150.0, scene.camera().nearPlaneMeters());
    EXPECT_DOUBLE_EQ(1e12, scene.camera().farPlaneMeters());
}
