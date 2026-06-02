#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/camera/CameraController.h"
#include "earth_engine/scene/Camera.h"

using namespace earth_engine;

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

    // 不应低于最小值
    controller_->setDistance(1.0f);
    EXPECT_GE(controller_->distance(), 2.4f);

    // 不应超过最大值
    controller_->setDistance(20.0f);
    EXPECT_LE(controller_->distance(), 12.0f);
}

TEST_F(CameraControllerTest, DragChangesRotation) {
    auto initialRotation = controller_->rotation();

    // 模拟从左到右的拖拽（经度方向旋转）
    controller_->onDragStart(200.0f, 300.0f);
    controller_->onDragMove(400.0f, 300.0f);
    controller_->onDragEnd();

    auto newRotation = controller_->rotation();

    // 旋转应发生变化
    bool rotationChanged = (std::abs(initialRotation.w - newRotation.w) > 1e-6) ||
                           (std::abs(initialRotation.x - newRotation.x) > 1e-6) ||
                           (std::abs(initialRotation.y - newRotation.y) > 1e-6) ||
                           (std::abs(initialRotation.z - newRotation.z) > 1e-6);
    EXPECT_TRUE(rotationChanged);
}

TEST_F(CameraControllerTest, DragThenInertiaDecays) {
    // 快速拖拽
    controller_->onDragStart(200.0f, 300.0f);
    controller_->onDragMove(600.0f, 300.0f);  // 大位移 → 高角速度
    controller_->onDragEnd();

    // 记录松开后的旋转
    auto q1 = controller_->rotation();

    // 模拟多帧更新（惯性应衰减到接近停止）
    for (int i = 0; i < 120; ++i) {
        controller_->update(1.0 / 60.0);
    }

    auto q2 = controller_->rotation();

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
    float initialDist = controller_->distance();

    controller_->onPinch(2.0f);  // 缩小 → 距离减小

    // 距离应显著减小
    EXPECT_LT(controller_->distance(), initialDist * 0.75f);

    controller_->onPinch(0.5f);  // 放大 → 距离增大

    // 距离应大于之前
    float distAfterZoomOut = controller_->distance();
    EXPECT_GT(distAfterZoomOut, initialDist * 0.5f);
}

TEST_F(CameraControllerTest, UpdateUpdatesCameraPosition) {
    // 初始相机 view matrix
    auto initialView = camera_->viewMatrix();

    // 拖拽后 update
    controller_->onDragStart(200.0f, 300.0f);
    controller_->onDragMove(500.0f, 100.0f);
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
