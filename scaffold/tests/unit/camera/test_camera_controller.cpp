#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include "earth_engine/camera/CameraController.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"

using namespace earth_engine;

namespace {

constexpr double kEarthRadiusMeters = 6378137.0;

double quatAngleFromIdentity(const glm::dquat& q) {
    glm::dquat n = glm::normalize(q);
    return 2.0 * std::acos(std::clamp(std::abs(n.w), 0.0, 1.0));
}

Vec3 intersectEarthSphere(const Ray& ray) {
    const glm::dvec3 o = ray.origin().raw();
    const glm::dvec3 d = ray.direction().raw();
    const double b = 2.0 * glm::dot(o, d);
    const double c = glm::dot(o, o) - kEarthRadiusMeters * kEarthRadiusMeters;
    const double disc = b * b - 4.0 * c;
    EXPECT_GE(disc, 0.0);
    const double t0 = (-b - std::sqrt(std::max(0.0, disc))) * 0.5;
    const double t1 = (-b + std::sqrt(std::max(0.0, disc))) * 0.5;
    const double t = t0 > 0.0 ? t0 : t1;
    EXPECT_GT(t, 0.0);
    return ray.pointAt(t);
}

glm::dvec2 projectToScreen(const Camera& camera, const Vec3& world) {
    constexpr double kViewportWidth = 800.0;
    constexpr double kViewportHeight = 600.0;
    const glm::dmat4 vp = camera.viewProjectionMatrix(kViewportWidth, kViewportHeight).raw();
    glm::dvec4 clip = vp * glm::dvec4(world.raw(), 1.0);
    clip /= clip.w;
    return {
        (clip.x + 1.0) * 0.5 * kViewportWidth,
        (1.0 - clip.y) * 0.5 * kViewportHeight
    };
}

double cameraSlope(const Camera& camera) {
    return camera.direction().dot(-camera.position().normalized());
}

} // namespace

class CameraControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(glm::radians(60.0), 1.0, 50000000.0);
        controller_ = std::make_unique<CameraController>(camera_.get());
        controller_->setViewport(800, 600);
    }

    std::unique_ptr<Camera> camera_;
    std::unique_ptr<CameraController> controller_;
};

TEST_F(CameraControllerTest, InitialState) {
    // 初始距离应为默认值 7.0 地球半径
    EXPECT_FLOAT_EQ(7.0f, controller_->distance());

    // 初始旋转应为归一化四元数；默认首屏面向东亚，便于检查 XYZ 底图。
    auto q = controller_->rotation();
    double len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    EXPECT_NEAR(1.0, len, 1e-6);
}

TEST_F(CameraControllerTest, SetDistance) {
    controller_->setDistance(5.0f);
    EXPECT_FLOAT_EQ(5.0f, controller_->distance());

    controller_->setDistance(1.0f);
    controller_->update(0.0);
    EXPECT_GE(camera_->position().length() - kEarthRadiusMeters, 49.0);
    EXPECT_LT(camera_->position().length() - kEarthRadiusMeters, 52.0);

    controller_->setDistance(20.0f);
    EXPECT_FLOAT_EQ(20.0f, controller_->distance());

    controller_->setDistance(40.0f);
    EXPECT_LE(controller_->distance(), 30.0f);
}

TEST_F(CameraControllerTest, DragChangesRotation) {
    auto initialRotation = controller_->rotation();

    // 模拟从屏幕中心抓住地球向右拖拽
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();

    auto newRotation = controller_->rotation();

    // 旋转应发生变化
    bool rotationChanged = (std::abs(initialRotation.w - newRotation.w) > 1e-6) ||
                           (std::abs(initialRotation.x - newRotation.x) > 1e-6) ||
                           (std::abs(initialRotation.y - newRotation.y) > 1e-6) ||
                           (std::abs(initialRotation.z - newRotation.z) > 1e-6);
    EXPECT_TRUE(rotationChanged);
}

TEST_F(CameraControllerTest, DragDirectionMatchesScreenDelta) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    EXPECT_GT(camera_->position().x(), 0.0);
    EXPECT_NEAR(0.0, camera_->position().y(), 1e-6);

    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(370.0f, 300.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    EXPECT_LT(camera_->position().x(), 0.0);
    EXPECT_NEAR(0.0, camera_->position().y(), 1e-6);

    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(400.0f, 270.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    EXPECT_LT(camera_->position().y(), 0.0);
    EXPECT_NEAR(0.0, camera_->position().x(), 1e-6);

    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(400.0f, 330.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    EXPECT_GT(camera_->position().y(), 0.0);
    EXPECT_NEAR(0.0, camera_->position().x(), 1e-6);
}

TEST_F(CameraControllerTest, DraggedSurfacePointFollowsFingerDirection) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    Vec3 grabbed = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    glm::dvec2 afterRight = projectToScreen(*camera_, grabbed);
    EXPECT_GT(afterRight.x, 400.0);
    EXPECT_NEAR(300.0, afterRight.y, 2.0);

    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    grabbed = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(400.0f, 270.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    glm::dvec2 afterUp = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(400.0, afterUp.x, 2.0);
    EXPECT_LT(afterUp.y, 300.0);
}

TEST_F(CameraControllerTest, DragStepUsesAnchorSphereIntersection) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(3.0f);
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();
    double nearAngle = quatAngleFromIdentity(controller_->rotation());

    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(9.0f);
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();
    double farAngle = quatAngleFromIdentity(controller_->rotation());

    EXPECT_GT(nearAngle, 0.0);
    EXPECT_GT(farAngle, nearAngle);
}

TEST_F(CameraControllerTest, DragUsesInjectedSurfacePicker) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);

    int pickCount = 0;
    controller_->setSurfacePicker([&](float x, float y, Vec3& outPoint) {
        ++pickCount;
        Ray ray = camera_->getPickRay(x, y, 800.0, 600.0);
        outPoint = intersectEarthSphere(ray) * 1.001;
        return true;
    });

    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();

    EXPECT_GE(pickCount, 2);
    EXPECT_GT(quatAngleFromIdentity(controller_->rotation()), 0.0);
}

TEST_F(CameraControllerTest, DragThenInertiaDecays) {
    // 快速拖拽
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);  // 有效球面内位移 → 惯性
    controller_->onDragEnd();

    // 模拟多帧更新（惯性应衰减到接近停止）
    for (int i = 0; i < 120; ++i) {
        controller_->update(1.0 / 60.0);
    }

    // 经过 2 秒后旋转应基本稳定（惯性衰减系数 e^(-3*2) ≈ 0.0025）
    // 但第一次 update 的旋转可能已经使 q1 != q2
    // 测试最后几帧的变化很小
    auto qBefore = controller_->rotation();
    controller_->update(1.0 / 60.0);
    auto qAfter = controller_->rotation();

    double rotDiff = std::abs(qBefore.w - qAfter.w) +
                     std::abs(qBefore.x - qAfter.x) +
                     std::abs(qBefore.y - qAfter.y) +
                     std::abs(qBefore.z - qAfter.z);
    EXPECT_LT(rotDiff, 0.01);
}

TEST_F(CameraControllerTest, PinchChangesDistance) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(5.0f);
    controller_->update(0.0);
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));
    double initialDist = camera_->position().distanceTo(anchor);

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(2.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);  // 缩小 → 距离减小

    EXPECT_LT(camera_->position().distanceTo(anchor), initialDist);
    glm::dvec2 projectedAfterZoomIn = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(400.0, projectedAfterZoomIn.x, 2.0);
    EXPECT_NEAR(300.0, projectedAfterZoomIn.y, 2.0);

    controller_->onPinchEnd();
    controller_->setDistance(5.0f);
    controller_->update(0.0);
    anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));
    initialDist = camera_->position().distanceTo(anchor);
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(0.5f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);  // 放大 → 距离增大

    EXPECT_GT(camera_->position().distanceTo(anchor), initialDist);
    glm::dvec2 projectedAfterZoomOut = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(400.0, projectedAfterZoomOut.x, 2.0);
    EXPECT_NEAR(300.0, projectedAfterZoomOut.y, 2.0);
}

TEST_F(CameraControllerTest, PinchRotationChangesCameraAroundAnchor) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    Vec3 grabbed = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.25f, 0.0f, 0.0f);
    controller_->update(0.0);

    glm::dvec2 projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(400.0, projected.x, 2.0);
    EXPECT_NEAR(300.0, projected.y, 2.0);
    EXPECT_GT(quatAngleFromIdentity(controller_->rotation()), 0.01);
}

TEST_F(CameraControllerTest, PinchZoomKeepsGestureCenterAnchorStable) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(5.0f);
    controller_->update(0.0);

    constexpr double anchorX = 470.0;
    constexpr double anchorY = 260.0;
    Vec3 centerPoint = intersectEarthSphere(
        camera_->getPickRay(anchorX, anchorY, 800.0, 600.0));
    const double beforeDistance = camera_->position().distanceTo(centerPoint);

    controller_->onPinchGesture(1.0f, static_cast<float>(anchorX), static_cast<float>(anchorY), 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.35f, static_cast<float>(anchorX), static_cast<float>(anchorY), 0.0f, 0.0f, 0.0f);
    controller_->update(0.0);

    const double afterDistance = camera_->position().distanceTo(centerPoint);
    EXPECT_LT(afterDistance, beforeDistance);
}

TEST_F(CameraControllerTest, PinchUsesInjectedSurfacePickerForCurrentCenter) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(5.0f);
    controller_->update(0.0);

    int pickCount = 0;
    controller_->setSurfacePicker([&](float x, float y, Vec3& outPoint) {
        ++pickCount;
        Ray ray = camera_->getPickRay(x, y, 800.0, 600.0);
        outPoint = intersectEarthSphere(ray) * 1.002;
        return true;
    });

    controller_->onPinchGesture(1.0f, 470.0f, 260.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.2f, 470.0f, 260.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchEnd();

    EXPECT_GE(pickCount, 2);
    EXPECT_LT(controller_->distance(), 5.0f);
}

TEST_F(CameraControllerTest, PinchZoomContinuesWhenCurrentCenterPickMisses) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(5.0f);
    controller_->update(0.0);

    int pickCount = 0;
    controller_->setSurfacePicker([&](float x, float y, Vec3& outPoint) {
        ++pickCount;
        if (pickCount > 2) {
            return false;
        }
        Ray ray = camera_->getPickRay(x, y, 800.0, 600.0);
        outPoint = intersectEarthSphere(ray);
        return true;
    });

    const float before = controller_->distance();
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.2f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->update(0.0);

    EXPECT_GE(pickCount, 2);
    EXPECT_LT(controller_->distance(), before);
}

TEST_F(CameraControllerTest, PinchZoomClampsToMinAltitudeInsteadOfStopping) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(1.01f);
    controller_->update(0.0);
    const double beforeAltitude =
        Ellipsoid::WGS84().cartesianToCartographic(camera_->position()).height();

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 24; ++i) {
        controller_->onPinchGesture(1.3f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    }
    controller_->update(0.0);

    const double altitude =
        Ellipsoid::WGS84().cartesianToCartographic(camera_->position()).height();
    EXPECT_GE(altitude, 49.0);
    EXPECT_LT(altitude, beforeAltitude);
}

TEST_F(CameraControllerTest, PinchRotationSignIsPredictable) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    Vec3 reference = intersectEarthSphere(
        camera_->getPickRay(400.0, 250.0, 800.0, 600.0));
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.25f, 0.0f, 0.0f);
    controller_->update(0.0);
    const glm::dvec2 positiveProjected = projectToScreen(*camera_, reference);

    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->onPinchEnd();
    controller_->update(0.0);
    reference = intersectEarthSphere(
        camera_->getPickRay(400.0, 250.0, 800.0, 600.0));
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, -0.25f, 0.0f, 0.0f);
    controller_->update(0.0);
    const glm::dvec2 negativeProjected = projectToScreen(*camera_, reference);

    EXPECT_GT(positiveProjected.x, 400.0);
    EXPECT_LT(negativeProjected.x, 400.0);
    EXPECT_NEAR(positiveProjected.y, negativeProjected.y, 1e-6);
}

TEST_F(CameraControllerTest, PinchTiltChangesCamera) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    auto before = camera_->viewMatrix();

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 400.0f, 260.0f, 0.0f, 0.0f, -40.0f);
    controller_->update(0.0);

    auto after = camera_->viewMatrix();
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (std::abs(before(i, j) - after(i, j)) > 1e-6) {
                changed = true;
            }
        }
    }
    EXPECT_TRUE(changed);
}

TEST_F(CameraControllerTest, PinchPushUpIncreasesTiltPullDownDecreasesTilt) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    const double beforePushUpSlope = cameraSlope(*camera_);

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 400.0f, 260.0f, 0.0f, 0.0f, -40.0f);
    controller_->update(0.0);
    const double afterPushUpSlope = cameraSlope(*camera_);

    EXPECT_LT(afterPushUpSlope, beforePushUpSlope);

    const double beforePullDownSlope = cameraSlope(*camera_);

    controller_->onPinchGesture(1.0f, 400.0f, 340.0f, 0.0f, 0.0f, 40.0f);
    controller_->update(0.0);
    const double afterPullDownSlope = cameraSlope(*camera_);

    EXPECT_GT(afterPullDownSlope, beforePullDownSlope);
}

TEST_F(CameraControllerTest, PinchHorizontalPanChangesCamera) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    auto before = camera_->viewMatrix();

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 430.0f, 300.0f, 0.0f, 30.0f, 0.0f);
    controller_->update(0.0);

    auto after = camera_->viewMatrix();
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (std::abs(before(i, j) - after(i, j)) > 1e-6) {
                changed = true;
            }
        }
    }
    EXPECT_TRUE(changed);
}

TEST_F(CameraControllerTest, ViewDistanceMovesCameraTowardPickedTarget) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(7.0f);
    controller_->update(0.0);

    Vec3 target = intersectEarthSphere(
        camera_->getPickRay(420.0, 280.0, 800.0, 600.0));
    const double requestedDistance = camera_->position().distanceTo(target) * 0.57;

    controller_->viewDistance(target, requestedDistance);

    EXPECT_NEAR(requestedDistance, camera_->position().distanceTo(target), 1e-3);
    EXPECT_GT(camera_->direction().dot((target - camera_->position()).normalized()), 0.999);
}

TEST_F(CameraControllerTest, ViewDistanceRespectsNearGroundSafetyFloor) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(7.0f);
    controller_->update(0.0);

    Vec3 target = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->viewDistance(target, 10.0);

    EXPECT_NEAR(50.0, camera_->position().distanceTo(target), 1e-3);
    EXPECT_GT(camera_->direction().dot((target - camera_->position()).normalized()), 0.999);
}

TEST_F(CameraControllerTest, UpdateUpdatesCameraPosition) {
    // 初始相机 view matrix
    auto initialView = camera_->viewMatrix();

    // 拖拽后 update
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 270.0f);
    controller_->onDragEnd();
    controller_->update(1.0 / 60.0);

    auto newView = camera_->viewMatrix();

    // view matrix 应变化
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            if (std::abs(initialView(i, j) - newView(i, j)) > 1e-6) {
                changed = true;
            }
        }
    }
    EXPECT_TRUE(changed);
}

TEST_F(CameraControllerTest, CameraLooksAtOrigin) {
    // 经过 update 后，相机应始终看向原点
    controller_->update(1.0 / 60.0);

    // 相机方向应指向原点
    Vec3 pos = camera_->position();
    Vec3 dir = camera_->direction();

    // pos + dir * t = origin for some t > 0
    // dir 应指向原点的反方向（pos 指向原点）
    double dot = pos.normalized().dot(dir);
    // 如果相机看向原点，pos 和 -dir 应同向，所以 pos · dir < 0
    EXPECT_LT(dot, 0.0);
}
