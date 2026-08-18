// C-P3:pin 解算的 host 单测(脱 GPU)。
//
// 语义不变量(区别于 test_camera_pose_trace 的整轨迹 hash):**手势后被钉的
// 世界点必须投影回手指像素**——即 anchorErr≈0。这正是真机 CAMPROBE 闭环
// (C-V1)在测的东西,搬到 host 纯几何跑,不依赖设备/GPU。
//
// 覆盖:grabSurfacePoint(内部 WGS84 球面拾取)/ solveAnchorRotation 良态分支 /
//       applyAnchorDrag / applyPinchPin(pin 保锚)。

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <cmath>
#include <memory>

#include "earth_engine/camera/CameraConstraintSolver.h"
#include "earth_engine/camera/controllers/FreeGlobeController.h"
#include "earth_engine/camera/controllers/TouchGesture.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"

using namespace earth_engine;

namespace {

constexpr int kW = 800;
constexpr int kH = 600;

// 世界点 → 屏幕像素(与 tools/cam_probe/camprobe.py 同一投影:VP·p,透视除,
// NDC→像素,y 翻转)。返回 false = 在相机后方。
bool projectToScreen(const Camera& cam, const glm::dvec3& world,
                     glm::dvec2& outPx) {
    const glm::dmat4 vp =
        cam.viewProjectionMatrix(double(kW), double(kH)).raw();
    const glm::dvec4 clip = vp * glm::dvec4(world, 1.0);
    if (clip.w <= 1e-9) return false;
    const glm::dvec3 ndc = glm::dvec3(clip) / clip.w;
    outPx.x = (ndc.x * 0.5 + 0.5) * kW;
    outPx.y = (1.0 - (ndc.y * 0.5 + 0.5)) * kH;
    return true;
}

// 高空 nadir 相机(Space 模式:pitch≈0<60° 且 alt≫150km ⇒ 良态锚定路径),
// 置于 (lonDeg,latDeg) 正上方 altMeters 处俯视。
void placeNadir(Camera& cam, double lonDeg, double latDeg, double altMeters) {
    const Ellipsoid& e = Ellipsoid::WGS84();
    const Vec3 surface =
        e.cartographicToCartesian(Cartographic::fromDegrees(lonDeg, latDeg, 0.0));
    const glm::dvec3 up = e.geodeticSurfaceNormal(surface).raw();  // 局部天顶
    const glm::dvec3 eye = surface.raw() + up * altMeters;
    // 北向切向作为相机 up;视线朝下(-天顶)。
    const glm::dvec3 zAxis(0.0, 0.0, 1.0);
    glm::dvec3 east = glm::cross(zAxis, up);
    east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
    const glm::dvec3 north = glm::normalize(glm::cross(up, east));
    cam.setView(Vec3(eye), Vec3(-up), Vec3(north));
}

class PinAnchorErrorTest : public ::testing::Test {
protected:
    void SetUp() override {
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(glm::radians(60.0), 1.0, 5.0e7);
        solver_ = std::make_unique<CameraConstraintSolver>();
        controller_ =
            std::make_unique<FreeGlobeController>(camera_.get(), solver_.get());
        controller_->setViewport(kW, kH);
        placeNadir(*camera_, 106.5, 29.6, 3.0e5);  // 300km 高空
    }

    // 当前被钉世界点的 anchorErr(px);无锚点返回 -1。
    double anchorErrAt(double fx, double fy) const {
        Vec3 anchor;
        if (!controller_->debugAnchorWorld(anchor)) return -1.0;
        glm::dvec2 px;
        if (!projectToScreen(*camera_, anchor.raw(), px)) return -1.0;
        return std::hypot(px.x - fx, px.y - fy);
    }

    std::unique_ptr<Camera> camera_;
    std::unique_ptr<CameraConstraintSolver> solver_;
    std::unique_ptr<FreeGlobeController> controller_;
};

// 单指拖拽:每一步 move 后,grabbed 世界点必投影回该步手指像素。
TEST_F(PinAnchorErrorTest, DragKeepsAnchorUnderFinger) {
    const float x0 = 400.0f, y0 = 300.0f;  // 屏幕中心,良态
    controller_->onDragStart(x0, y0, 0.0);

    Vec3 anchor0;
    ASSERT_TRUE(controller_->debugAnchorWorld(anchor0))
        << "起手没抓到锚点(球面拾取失败)";
    EXPECT_LT(anchorErrAt(x0, y0), 0.5) << "起手 anchorErr 应≈0";

    // 一串斜向 move:每步锚点都要跟到指下。
    for (int i = 1; i <= 12; ++i) {
        const float fx = x0 + 12.0f * i;
        const float fy = y0 + 7.0f * i;
        controller_->onDragMove(fx, fy, i * 0.016);
        const double err = anchorErrAt(fx, fy);
        ASSERT_GE(err, 0.0) << "第 " << i << " 步锚点丢失";
        EXPECT_LT(err, 0.5)
            << "第 " << i << " 步 anchorErr=" << err << "px 超阈(pin 泄漏)";
    }
}

// 双指纯缩放:pin 目标=质心,缩放全程锚点钉在质心不漂。
TEST_F(PinAnchorErrorTest, PinchKeepsAnchorAtCentroid) {
    const float cx = 400.0f, cy = 300.0f;

    PinchInput in{};
    in.centroidX = cx;
    in.centroidY = cy;
    in.scaleFromStart = 1.0f;
    in.twistFromStartRadians = 0.0f;
    in.timestamp = 0.0;
    controller_->onPinchGesture(in);  // 起手钉质心

    ASSERT_TRUE(controller_->pinching());
    EXPECT_LT(anchorErrAt(cx, cy), 0.5) << "pinch 起手锚点未钉质心";

    // 逐步放大:质心不动,anchorErr 应始终≈0。
    for (int i = 1; i <= 10; ++i) {
        in.scaleFromStart = 1.0f + 0.12f * i;
        in.timestamp = i * 0.016;
        controller_->onPinchGesture(in);
        const double err = anchorErrAt(cx, cy);
        ASSERT_GE(err, 0.0) << "第 " << i << " 步 pinch 锚点丢失";
        EXPECT_LT(err, 0.5)
            << "第 " << i << " 步 pinch anchorErr=" << err << "px(缩放漏锚)";
    }
}

}  // namespace
