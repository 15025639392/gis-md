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

// 近地正常锚点倾斜:45° 下俯、锚点 ~1.4km(在海拔自适应上限 2×1km=2km 内),
// 锚点被接受,双指倾斜正常工作且不跑飞。
TEST_F(PinAnchorErrorTest, NearGroundAcceptedAnchorTiltWorksNoRunaway) {
    const auto& e = Ellipsoid::WGS84();
    const Vec3 ground = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    const glm::dvec3 up = e.geodeticSurfaceNormal(ground).raw();
    const glm::dvec3 eye = ground.raw() + up * 1000.0;  // 1km AGL
    const glm::dvec3 east =
        glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
    const glm::dvec3 dir = glm::normalize(
        east * std::cos(glm::radians(45.0)) - up * std::sin(glm::radians(45.0)));
    const glm::dvec3 camUp =
        glm::normalize(glm::cross(dir, glm::cross(up, dir)));
    camera_->setView(Vec3(eye), Vec3(dir), Vec3(camUp));

    // 拾取返回椭球入口(远处低半径点 → 抓取球半径≈R → 锚点远)。
    controller_->setSurfacePicker(
        [this](float x, float y, Vec3& out) -> bool {
            const Ray ray = camera_->getPickRay(
                double(x), double(y), double(kW), double(kH));
            const auto& ell = Ellipsoid::WGS84();
            const std::optional<Vec3> hit =
                ell.rayIntersection(ray.origin(), ray.direction());
            if (!hit) return false;
            out = *hit;
            return true;
        });

    PinchInput in{};
    in.centroidX = 400.0f;
    in.centroidY = 300.0f;
    in.scaleFromStart = 1.0f;
    in.timestamp = 0.0;
    in.mode = PinchMode::Undecided;
    controller_->onPinchGesture(in);  // 起手钉锚

    Vec3 anchor;
    ASSERT_TRUE(controller_->debugAnchorWorld(anchor));
    const double anchorDist =
        glm::length(anchor.raw() - camera_->position().raw());
    ASSERT_GT(anchorDist, 500.0)
        << "场景失效:锚点太近,倾斜位移无意义";
    ASSERT_LT(anchorDist, 2500.0)
        << "场景失效:锚点应被海拔自适应上限接受(2km 内)";

    // Pitch 模式,质心逐步下移触发倾斜。
    in.mode = PinchMode::Pitch;
    const glm::dvec3 eyeBefore = camera_->position().raw();
    glm::dvec3 prevEye = camera_->position().raw();
    double maxSwing = 0.0;
    for (int i = 1; i <= 12; ++i) {
        in.centroidY = 300.0f + 20.0f * i;
        in.timestamp = i * 0.016;
        controller_->onPinchGesture(in);
        const glm::dvec3 now = camera_->position().raw();
        maxSwing = std::max(maxSwing, glm::length(now - prevEye));
        prevEye = now;
    }
    // 倾斜确实发生了(绕锚点转 ⇒ 相机位置有位移;避免场景空转)。
    EXPECT_GT(glm::length(camera_->position().raw() - eyeBefore), 5.0)
        << "场景失效:倾斜没发生(锚点被拒/守卫拒绝)";
    // 近锚倾斜单步位移由 0.08 rad 步长上限决定:1.4km×0.08≈112m/步,
    // 断言 ≤200m 防止退化区跑飞(修复前远锚单步 200km)。
    EXPECT_LE(maxSwing, 200.0)
        << "近锚倾斜单步位移过大:退化区跑飞(跳远)";
}

// 掠射/远锚拒绝:近水平视线(2° 下俯)下拾取点 ~30km 外、条件数≈0.03<0.1,
// 起手必须拒绝;中途质心下移让射线变陡、重试获取也不得把远锚(>2×海拔)
// 拉回来——修复前单步 200km=跳远。拒绝后走无锚路径,捏合(不缩放)不动相机。
TEST_F(PinAnchorErrorTest, GrazingOrFarAnchorRejectedNoRunaway) {
    const auto& e = Ellipsoid::WGS84();
    const Vec3 ground = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    const glm::dvec3 up = e.geodeticSurfaceNormal(ground).raw();
    const glm::dvec3 eye = ground.raw() + up * 1000.0;
    const glm::dvec3 east =
        glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
    const glm::dvec3 dir = glm::normalize(
        east * std::cos(glm::radians(2.0)) - up * std::sin(glm::radians(2.0)));
    const glm::dvec3 camUp =
        glm::normalize(glm::cross(dir, glm::cross(up, dir)));
    camera_->setView(Vec3(eye), Vec3(dir), Vec3(camUp));

    controller_->setSurfacePicker(
        [this](float x, float y, Vec3& out) -> bool {
            const Ray ray = camera_->getPickRay(
                double(x), double(y), double(kW), double(kH));
            const auto& ell = Ellipsoid::WGS84();
            const std::optional<Vec3> hit =
                ell.rayIntersection(ray.origin(), ray.direction());
            if (!hit) return false;
            out = *hit;
            return true;
        });

    PinchInput in{};
    in.centroidX = 400.0f;
    in.centroidY = 300.0f;
    in.scaleFromStart = 1.0f;
    in.timestamp = 0.0;
    in.mode = PinchMode::Undecided;
    controller_->onPinchGesture(in);

    // 掠射锚点必须被拒绝:无锚可钉。
    Vec3 anchor;
    EXPECT_FALSE(controller_->debugAnchorWorld(anchor))
        << "掠射锚点没被拒绝(条件数≈0.03 仍接受,会跳远)";

    // Pitch 手势(不缩放):起手拒绝 + 中途重试也不得拉回远锚,相机不得移动。
    in.mode = PinchMode::Pitch;
    const glm::dvec3 eyeBefore = camera_->position().raw();
    double maxSwing = 0.0;
    glm::dvec3 prevEye = eyeBefore;
    for (int i = 1; i <= 8; ++i) {
        in.centroidY = 300.0f + 20.0f * i;
        in.timestamp = i * 0.016;
        controller_->onPinchGesture(in);
        const glm::dvec3 now = camera_->position().raw();
        maxSwing = std::max(maxSwing, glm::length(now - prevEye));
        prevEye = now;
    }
    EXPECT_LE(maxSwing, 55.0)
        << "掠射/远锚态捏合仍产生大位移:锚点拒绝或重试守卫没生效";
    EXPECT_FALSE(controller_->debugAnchorWorld(anchor))
        << "中途重试把远锚拉回来了";
}

// 高空球心回中(契约 2.4):拉远到地球可见(≥1.5R)后,捏合拉远应把视轴转向地心,
// 让球心自然回到屏幕中心;4R 以上完全对准。
TEST_F(PinAnchorErrorTest, ZoomOutGlobeCentersAboveThreshold) {
    const auto& e = Ellipsoid::WGS84();
    const Vec3 surface = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    const glm::dvec3 up = e.geodeticSurfaceNormal(surface).raw();
    const glm::dvec3 eye = surface.raw() + up * 4.0 * 6378137.0;  // 4R
    const glm::dvec3 dir =
        glm::normalize(surface.raw() - eye);  // 指向地表点,偏离地心方向
    const glm::dvec3 camUp =
        glm::normalize(glm::cross(dir, glm::cross(up, dir)));
    camera_->setView(Vec3(eye), Vec3(dir), Vec3(camUp));

    controller_->setSurfacePicker(
        [this](float x, float y, Vec3& out) -> bool {
            const Ray ray = camera_->getPickRay(
                double(x), double(y), double(kW), double(kH));
            const auto& ell = Ellipsoid::WGS84();
            const std::optional<Vec3> hit =
                ell.rayIntersection(ray.origin(), ray.direction());
            if (!hit) return false;
            out = *hit;
            return true;
        });

    PinchInput in{};
    in.centroidX = 400.0f;
    in.centroidY = 300.0f;
    in.scaleFromStart = 1.0f;
    in.timestamp = 0.0;
    controller_->onPinchGesture(in);  // 起手
    in.scaleFromStart = 0.5f;          // 拉远 2×
    in.timestamp = 0.016;
    controller_->onPinchGesture(in);

    const glm::dvec3 dirAfter = camera_->direction().raw();
    const glm::dvec3 toCenter =
        glm::normalize(-camera_->position().raw());
    EXPECT_GT(glm::dot(dirAfter, toCenter), 0.999)
        << "4R 拉远后视轴未对准地心:高空球心回中没生效";
}

// 近地(<1.5R)缩放不得改变视轴方向(锚点钉指语义保持,不回中)。
TEST_F(PinAnchorErrorTest, ZoomNearGroundKeepsViewDirection) {
    const auto& e = Ellipsoid::WGS84();
    const Vec3 surface = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    const glm::dvec3 up = e.geodeticSurfaceNormal(surface).raw();
    const glm::dvec3 eye = surface.raw() + up * 1000.0;  // 1km AGL
    const glm::dvec3 dir =
        glm::normalize(surface.raw() - eye);
    const glm::dvec3 camUp =
        glm::normalize(glm::cross(dir, glm::cross(up, dir)));
    camera_->setView(Vec3(eye), Vec3(dir), Vec3(camUp));

    controller_->setSurfacePicker(
        [this](float x, float y, Vec3& out) -> bool {
            const Ray ray = camera_->getPickRay(
                double(x), double(y), double(kW), double(kH));
            const auto& ell = Ellipsoid::WGS84();
            const std::optional<Vec3> hit =
                ell.rayIntersection(ray.origin(), ray.direction());
            if (!hit) return false;
            out = *hit;
            return true;
        });

    PinchInput in{};
    in.centroidX = 400.0f;
    in.centroidY = 300.0f;
    in.scaleFromStart = 1.0f;
    in.timestamp = 0.0;
    controller_->onPinchGesture(in);
    const glm::dvec3 dirBefore = camera_->direction().raw();
    in.scaleFromStart = 0.5f;  // 拉远
    in.timestamp = 0.016;
    controller_->onPinchGesture(in);

    const glm::dvec3 dirAfter = camera_->direction().raw();
    EXPECT_GT(glm::dot(dirBefore, dirAfter), 0.9999)
        << "近地缩放改变了视轴方向:回中混入低空,破坏锚点钉指";
}

}  // namespace
