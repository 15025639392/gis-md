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

CameraController::PinchInput pinchIn(
    float scaleFromStart, float twistRadians, float cx, float cy,
    CameraController::PinchMode mode, double timestamp = 0.0) {
    CameraController::PinchInput input;
    input.scaleFromStart = scaleFromStart;
    input.twistFromStartRadians = twistRadians;
    input.centroidX = cx;
    input.centroidY = cy;
    input.mode = mode;
    input.timestamp = timestamp;
    return input;
}

// 解析式地形假体:把探针的 ENU 偏移换算回经纬(与生产 sampleAreaHeights 同法),
// 高度由 heightFn(lonRad, latRad) 给出,全点有效。
CameraController::TerrainAreaSampleFunc makeAnalyticAreaSampler(
    std::function<double(double, double)> heightFn,
    int* callCounter = nullptr) {
    return [heightFn, callCounter](
               const Vec3& groundEcef, double /*radiusMeters*/,
               const std::vector<glm::dvec2>& offsets,
               std::vector<CameraController::TerrainSample>& out) {
        if (callCounter) ++(*callCounter);
        const auto& e = Ellipsoid::WGS84();
        const Cartographic c = e.cartesianToCartographic(groundEcef);
        const double cosLat =
            std::max(std::abs(std::cos(c.latitude())), 0.01);
        out.assign(offsets.size(), {});
        for (size_t i = 0; i < offsets.size(); ++i) {
            const double lat = c.latitude() + offsets[i].y / kEarthRadiusMeters;
            const double lon =
                c.longitude() + offsets[i].x / (kEarthRadiusMeters * cosLat);
            const double h = heightFn(lon, lat);
            out[i].valid = true;
            out[i].heightMeters = h;
            out[i].surfaceEcef =
                e.cartographicToCartesian(Cartographic(lon, lat, h));
        }
    };
}

// 旧 orbit 摆位的等价物(orbit 表示已删):eye = -(q·+Z)·d·R,lookAt 地心,
// up = q·+Y。原来靠 setRotation()+setDistance() 翻开 orbit 模式再由 update()
// 每帧重建;现在一次性写位姿,语义逐位等价。
void placeOrbit(Camera& camera, const glm::dquat& q, double earthRadii) {
    const glm::dvec3 dir = q * glm::dvec3(0.0, 0.0, 1.0);
    const glm::dvec3 up = q * glm::dvec3(0.0, 1.0, 0.0);
    camera.lookAt(Vec3(-dir * earthRadii * kEarthRadiusMeters), Vec3::zero(),
                  Vec3(up));
}

void placeOrbitIdentity(Camera& camera, double earthRadii) {
    placeOrbit(camera, glm::dquat(1.0, 0.0, 0.0, 0.0), earthRadii);
}

double matrixAbsDiff(const Mat4& a, const Mat4& b) {
    double diff = 0.0;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            diff += std::abs(a(i, j) - b(i, j));
        }
    }
    return diff;
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
    // 构造即摆到重庆上空 1500m 看向地表。orbit 表示已删,不再有"默认 7 地球
    // 半径"这回事——distance() 现在是 |eye|/R 的派生视图。
    const auto& e = Ellipsoid::WGS84();
    const Cartographic c = e.cartesianToCartographic(camera_->position());
    EXPECT_NEAR(106.508, glm::degrees(c.longitude()), 1e-6);
    EXPECT_NEAR(29.617, glm::degrees(c.latitude()), 1e-6);
    EXPECT_NEAR(1500.0, c.height(), 1e-3);

    // 派生朝向四元数恒归一。
    const glm::dquat q = controller_->rotation();
    const double len = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    EXPECT_NEAR(1.0, len, 1e-9);
}

TEST_F(CameraControllerTest, DragKeepsGrabbedSurfacePointUnderFinger) {
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);
    Vec3 grabbed = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();
    controller_->update(0.0);

    glm::dvec2 projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(430.0, projected.x, 2.0);
    EXPECT_NEAR(300.0, projected.y, 2.0);
}

TEST_F(CameraControllerTest, DragKeepsGrabbedSurfacePointUnderFingerInAllDirections) {
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);
    Vec3 grabbed = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    glm::dvec2 projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(430.0, projected.x, 2.0);
    EXPECT_NEAR(300.0, projected.y, 2.0);

    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);
    grabbed = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(370.0f, 300.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(370.0, projected.x, 2.0);
    EXPECT_NEAR(300.0, projected.y, 2.0);

    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);
    grabbed = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(400.0f, 270.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(400.0, projected.x, 2.0);
    EXPECT_NEAR(270.0, projected.y, 2.0);

    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);
    grabbed = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(400.0f, 330.0f);
    controller_->onDragEnd();
    controller_->update(0.0);
    projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(400.0, projected.x, 2.0);
    EXPECT_NEAR(330.0, projected.y, 2.0);
}

TEST_F(CameraControllerTest, DraggedSurfacePointFollowsFingerDirection) {
    placeOrbitIdentity(*camera_, 7.0);
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

    placeOrbitIdentity(*camera_, 7.0);
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
    placeOrbitIdentity(*camera_, 3.0);
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();
    double nearAngle = quatAngleFromIdentity(controller_->rotation());

    placeOrbitIdentity(*camera_, 9.0);
    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);
    controller_->onDragEnd();
    double farAngle = quatAngleFromIdentity(controller_->rotation());

    EXPECT_GT(nearAngle, 0.0);
    EXPECT_GT(farAngle, nearAngle);
}

TEST_F(CameraControllerTest, DragUsesInjectedSurfacePicker) {
    placeOrbitIdentity(*camera_, 7.0);
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

    // move 期锚定抓取球面不重 pick 地形（跟手性），picker 只在 dragStart 调用。
    EXPECT_EQ(pickCount, 1);
    EXPECT_GT(quatAngleFromIdentity(controller_->rotation()), 0.0);
}

TEST_F(CameraControllerTest, DragThenInertiaDecays) {
    controller_->onDragStart(400.0f, 300.0f, 1.0);
    controller_->onDragMove(430.0f, 300.0f, 1.016);
    controller_->onDragEnd();

    for (int i = 0; i < 120; ++i) {
        controller_->update(1.0 / 60.0);
    }

    auto qBefore = controller_->rotation();
    controller_->update(1.0 / 60.0);
    auto qAfter = controller_->rotation();

    double rotDiff = std::abs(qBefore.w - qAfter.w) +
                     std::abs(qBefore.x - qAfter.x) +
                     std::abs(qBefore.y - qAfter.y) +
                     std::abs(qBefore.z - qAfter.z);
    EXPECT_LT(rotDiff, 0.01);
}

TEST_F(CameraControllerTest, DragSpinsWhenPointerMissesGlobeInsteadOfDeadZone) {
    // 回归（A0 消死区）：手指按在地球轮廓外（pick ray 打空）时，旧实现
    // onDragStart 直接放弃整段拖拽（dragging_=false → onDragMove no-op），
    // 地图卡死不动。现在应回退到 spin 转台旋转。
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);

    // 先确认起点确实在球面之外（判别式 < 0 → ray 不命中），否则测不到回退。
    const Ray missRay = camera_->getPickRay(780.0, 40.0, 800.0, 600.0);
    const glm::dvec3 o = missRay.origin().raw();
    const glm::dvec3 d = missRay.direction().raw();
    const double b = 2.0 * glm::dot(o, d);
    const double c = glm::dot(o, o) - kEarthRadiusMeters * kEarthRadiusMeters;
    ASSERT_LT(b * b - 4.0 * c, 0.0);

    controller_->onDragStart(780.0f, 40.0f);
    controller_->onDragMove(740.0f, 90.0f);
    controller_->onDragEnd();

    EXPECT_GT(quatAngleFromIdentity(controller_->rotation()), 1e-6);
}

TEST_F(CameraControllerTest, DragSpinFlickOverEmptySpaceSeedsInertia) {
    // spin 与 anchor 共用惯性通道：隔着地平线甩一下，松手后应继续滑行。
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);

    controller_->onDragStart(780.0f, 40.0f, 1.0);
    controller_->onDragMove(720.0f, 100.0f, 1.016);
    controller_->onDragEnd();

    const auto qEnd = controller_->rotation();
    controller_->update(1.0 / 60.0);
    const auto qGlide = controller_->rotation();
    const double diff = std::abs(qEnd.w - qGlide.w) + std::abs(qEnd.x - qGlide.x) +
                        std::abs(qEnd.y - qGlide.y) + std::abs(qEnd.z - qGlide.z);
    EXPECT_GT(diff, 1e-9);
}

TEST_F(CameraControllerTest, DragAcrossLimbHasNoScreenSpaceJump) {
    // N9（问题3 根治的连续性判据）：手指从球心划到球缘外，锚定→转台的
    // 过渡必须连续。判据用屏幕空间：每个 20px 步长里，屏幕中心地表参照点
    // 的位移不得超过 3.5×步长（旧 latch 实现重入球面时会把过期锚点猛拉回
    // 指下，单事件位移可达数百 px）。
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);

    controller_->onDragStart(400.0f, 300.0f);
    float x = 400.0f;
    const float step = 20.0f;
    for (int i = 0; i < 18; ++i) {
        const Vec3 ref = intersectEarthSphere(
            camera_->getPickRay(400.0, 300.0, 800.0, 600.0));
        x += step;
        controller_->onDragMove(x, 300.0f);
        const glm::dvec2 p = projectToScreen(*camera_, ref);
        const double moved = std::hypot(p.x - 400.0, p.y - 300.0);
        EXPECT_LT(moved, 3.5 * step) << "step " << i << " x=" << x;
    }
    controller_->onDragEnd();
}

TEST_F(CameraControllerTest, DragReentersGlobeRestoresAnchorFollow) {
    // N10（问题3 回归）：拖出球缘再拖回来，锚定必须恢复——旧实现一旦 miss
    // 即 latch 到转台直到抬手，重入球面后整段退化为非锚定旋转。
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);

    controller_->onDragStart(400.0f, 300.0f);
    controller_->onDragMove(500.0f, 300.0f);  // 出球缘（球屏半径 ~75px）
    controller_->onDragMove(600.0f, 300.0f);
    controller_->onDragMove(700.0f, 300.0f);
    controller_->onDragMove(600.0f, 300.0f);  // 折返
    controller_->onDragMove(500.0f, 300.0f);
    controller_->onDragMove(430.0f, 300.0f);  // 回到球内

    // 锚定已恢复：此时指下地表点在后续移动中应精确跟随手指。
    const Vec3 under = intersectEarthSphere(
        camera_->getPickRay(430.0, 300.0, 800.0, 600.0));
    controller_->onDragMove(460.0f, 300.0f);
    const glm::dvec2 p = projectToScreen(*camera_, under);
    EXPECT_NEAR(460.0, p.x, 2.0);
    EXPECT_NEAR(300.0, p.y, 2.0);
    controller_->onDragEnd();
}

TEST_F(CameraControllerTest, PinchChangesDistance) {
    placeOrbitIdentity(*camera_, 5.0);
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
    placeOrbitIdentity(*camera_, 5.0);
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

TEST_F(CameraControllerTest, PinchCombinedZoomAndRotateKeepsAnchorPinned) {
    // A1 锚点锁：缩放+旋转同时发生时，手指下的地表点必须保持在双指中心。
    placeOrbitIdentity(*camera_, 5.0);
    controller_->update(0.0);

    constexpr double cx = 450.0;
    constexpr double cy = 320.0;
    Vec3 grabbed = intersectEarthSphere(
        camera_->getPickRay(cx, cy, 800.0, 600.0));

    controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.2f, cx, cy, 0.15f, 0.0f, 0.0f);
    controller_->update(0.0);

    glm::dvec2 projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(cx, projected.x, 2.0);
    EXPECT_NEAR(cy, projected.y, 2.0);
}

TEST_F(CameraControllerTest, PinchZoomMagnitudeMatchesFingerSpread) {
    // 跟手判据：双指分开 s 倍，画面就该放大 s 倍——地表两点的屏幕间距比值 = s。
    // 旧实现沿视线移动 d*(s-1)（应为朝锚点移动 d*(1-1/s)），有效缩放变成
    // 1/(2-s)：s=1.25 时实际 1.333，超出手指 6.7%，且质心偏离屏幕中心时还带
    // 一阶横向漂移。质心刻意取在屏幕中心之外以覆盖后者。
    placeOrbitIdentity(*camera_, 5.0);
    controller_->update(0.0);

    constexpr double cx = 460.0;
    constexpr double cy = 350.0;
    const Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(cx, cy, 800.0, 600.0));
    // 探针朝屏幕中心一侧取，避免落到球缘外（distance=5R 时球只占屏幕中央一块）。
    const Vec3 probe = intersectEarthSphere(
        camera_->getPickRay(cx - 40.0, cy - 20.0, 800.0, 600.0));
    const double spreadBefore =
        glm::length(projectToScreen(*camera_, probe) -
                    projectToScreen(*camera_, anchor));

    constexpr float kScale = 1.25f;
    controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(kScale, cx, cy, 0.0f, 0.0f, 0.0f);
    controller_->update(0.0);

    const double spreadAfter =
        glm::length(projectToScreen(*camera_, probe) -
                    projectToScreen(*camera_, anchor));
    EXPECT_NEAR(kScale, spreadAfter / spreadBefore, 0.025);

    // 锚点仍钉在双指中心。
    const glm::dvec2 projected = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(cx, projected.x, 2.0);
    EXPECT_NEAR(cy, projected.y, 2.0);
}

TEST_F(CameraControllerTest, PinchJerkClampCarriesResidualInsteadOfDroppingIt) {
    // 单事件 jerk 限幅只该把突跳摊到相邻事件上，不该丢量：一次 s=2.0 的粗事件
    // 之后跟若干静止事件，累计缩放必须收敛到 2.0。旧实现直接夹到 1.3 并丢弃
    // 余量——快速捏合永久少缩放一截。
    placeOrbitIdentity(*camera_, 5.0);
    controller_->update(0.0);

    constexpr double cx = 400.0;
    constexpr double cy = 300.0;
    const Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(cx, cy, 800.0, 600.0));
    const double distBefore = camera_->position().distanceTo(anchor);

    controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(2.0f, cx, cy, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 4; ++i) {  // 残差在限幅余量内逐事件补回
        controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f);
    }
    controller_->update(0.0);

    const double distAfter = camera_->position().distanceTo(anchor);
    EXPECT_NEAR(2.0, distBefore / distAfter, 0.04);
}

TEST_F(CameraControllerTest, PinchSlowRotateRespondsAndKeepsAnchorPinned) {
    // A1：缓慢拧动（每帧远小于旧 0.003rad 死区）不应被死区吞掉——相机必须
    // 逐帧响应，同时地表锚点保持在双指中心。旧实现下 20×0.001rad 全被丢弃，
    // 相机纹丝不动（steppy），此断言 (a) 会失败。
    placeOrbitIdentity(*camera_, 5.0);
    controller_->update(0.0);

    constexpr double cx = 430.0;
    constexpr double cy = 330.0;
    Vec3 grabbed = intersectEarthSphere(
        camera_->getPickRay(cx, cy, 800.0, 600.0));

    controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 20; ++i) {
        controller_->onPinchGesture(1.0f, cx, cy, 0.001f, 0.0f, 0.0f);
    }
    controller_->update(0.0);

    // (a) 响应性：累计 20×0.001=0.02rad 的拧动必须真的转动相机。
    EXPECT_GT(quatAngleFromIdentity(controller_->rotation()), 0.01);
    // (b) 锚点锁：拧动全程地表点保持在双指中心。
    glm::dvec2 projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(cx, projected.x, 2.0);
    EXPECT_NEAR(cy, projected.y, 2.0);
}

TEST_F(CameraControllerTest, PinchZoomOffCenterKeepsAnchorPinnedWithoutSwing) {
    // A1 稳定性：在远离屏幕中心处捏合缩放时，手指下的地表点必须原地不动。
    // 旧实现沿 camera.direction() 前移再靠 keepAnchor 旋转回拉，off-center 时
    // 会先把锚点甩偏再纠正 → 首帧可见"瞬间偏移"。此处用 1px 严紧容差复现。
    placeOrbitIdentity(*camera_, 3.0);
    controller_->update(0.0);

    constexpr double ax = 520.0;  // 明显偏离中心 (400,300)
    constexpr double ay = 225.0;
    Vec3 grabbed = intersectEarthSphere(
        camera_->getPickRay(ax, ay, 800.0, 600.0));

    controller_->onPinchGesture(1.0f, ax, ay, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.2f, ax, ay, 0.0f, 0.0f, 0.0f);
    controller_->update(0.0);

    glm::dvec2 projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(ax, projected.x, 1.0);
    EXPECT_NEAR(ay, projected.y, 1.0);
}

TEST_F(CameraControllerTest, PinchZoomWithTerrainPickerKeepsAnchorPinned) {
    // 复现真机"瞬间偏移"：抓取锚点走 surfacePicker(地形，真实高度),而 keepAnchor
    // 走裸 intersectGrabSphere(球面)。两个面不一致时,首个缩放帧会按地形-球面
    // 夹角把地球猛地旋一下。这里注入一个"抬高"的 picker 模拟地形。
    placeOrbitIdentity(*camera_, 3.0);
    controller_->update(0.0);

    // 地形 picker：沿 pick ray 命中球面后径向抬高 ~30km(夸张但同量级于夹角效应)。
    controller_->setSurfacePicker([&](float x, float y, Vec3& outPoint) {
        Ray ray = camera_->getPickRay(x, y, 800.0, 600.0);
        Vec3 onSphere = intersectEarthSphere(ray);
        outPoint = onSphere * (1.0 + 30000.0 / kEarthRadiusMeters);
        return true;
    });

    constexpr double ax = 500.0;
    constexpr double ay = 240.0;
    Vec3 grabbed;  // 用同一 picker 记录被抓地表点
    {
        Ray ray = camera_->getPickRay(ax, ay, 800.0, 600.0);
        Vec3 onSphere = intersectEarthSphere(ray);
        grabbed = onSphere * (1.0 + 30000.0 / kEarthRadiusMeters);
    }

    controller_->onPinchGesture(1.0f, ax, ay, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.2f, ax, ay, 0.0f, 0.0f, 0.0f);
    controller_->update(0.0);

    glm::dvec2 projected = projectToScreen(*camera_, grabbed);
    EXPECT_NEAR(ax, projected.x, 1.5);
    EXPECT_NEAR(ay, projected.y, 1.5);
}

namespace {

// 视线偏离地心的夹角 θ：θ=0 ⟺ 球心恰在屏幕中心。
double offCenterAngle(const Camera& camera) {
    const glm::dvec3 toCenter = -glm::normalize(camera.position().raw());
    const glm::dvec3 dir = glm::normalize(camera.direction().raw());
    return std::acos(std::clamp(glm::dot(dir, toCenter), -1.0, 1.0));
}

// 视线(直线)到地心的垂距：dolly 与绕地心旋转(锚点跟手)都严格保持它，
// 只有高空回中会缩小它 → 作为回中是否介入的无噪声二元信号。
double viewLineMissDistance(const Camera& camera) {
    return glm::length(glm::cross(camera.position().raw(),
                                  glm::normalize(camera.direction().raw())));
}

// 倾斜视角初始位姿：eye 在 +X 轴上指定海拔，视线在 xz 平面内偏离地心 tiltDeg。
void setTiltedPose(Camera& camera, double altitudeMeters, double tiltDeg) {
    const double r = kEarthRadiusMeters + altitudeMeters;
    const glm::dvec3 eye(r, 0.0, 0.0);
    const double t = tiltDeg * glm::pi<double>() / 180.0;
    const glm::dvec3 dir(-std::cos(t), 0.0, std::sin(t));
    camera.lookAt(Vec3(eye), Vec3(eye + dir * 1.0e6), Vec3(0.0, 1.0, 0.0));
}

}  // namespace

TEST_F(CameraControllerTest, HighAltitudeZoomOutRecentersGlobeToScreenCenter) {
    // 高空拉远时球心应随缩放进度逐步回到屏幕中心（视线收敛向地心）。
    setTiltedPose(*camera_, 3.0e6, 15.0);
    const double theta0 = offCenterAngle(*camera_);
    const double miss0 = viewLineMissDistance(*camera_);
    ASSERT_GT(theta0, 0.2);  // ~15°

    // 锚点必须取屏幕中心：本用例把"视线到地心垂距"当作回中是否介入的无噪声
    // 信号，而该信号成立的前提是 dolly 沿视线（见上方 viewLineMissDistance
    // 注释）。锚点钉住的 dolly 是沿 eye→anchor 的，只有锚点在屏幕中心时两者
    // 重合；旧的 off-center 取值下，θ 的收敛大部分来自"沿视线后退"这一被动
    // 效应而非回中本身，信号被污染。off-center 的锚点跟手由 Pinch* 系列用例覆盖。
    constexpr float ax = 400.0f;
    constexpr float ay = 300.0f;
    controller_->onPinchGesture(1.0f, ax, ay, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 40; ++i) {
        controller_->onPinchGesture(0.93f, ax, ay, 0.0f, 0.0f, 0.0f);
    }
    // 手势期严格正交：回中只充值预算不动相机，θ 在手势内不因回中收敛。
    controller_->onPinchEnd();
    // 松手后沉降：update() 按指数节奏消费预算（~0.3-0.5s 收敛）。
    for (int i = 0; i < 120; ++i) {
        controller_->update(1.0 / 60.0);
    }

    const double thetaFinal = offCenterAngle(*camera_);
    EXPECT_LT(thetaFinal, 0.1 * theta0);
    EXPECT_LT(viewLineMissDistance(*camera_), 0.5 * miss0);

    // 球心投影落在屏幕中心附近。
    const glm::dvec2 centerPx = projectToScreen(*camera_, Vec3::zero());
    EXPECT_NEAR(400.0, centerPx.x, 30.0);
    EXPECT_NEAR(300.0, centerPx.y, 30.0);
}

TEST_F(CameraControllerTest, PinchRecenterDoesNotFightAnchorDuringGesture) {
    // N8：手势期回中与钉锚严格正交——zoom-out 手势事件内 viewLineMissDistance
    // 是精确不变量（dolly 沿视线/绕地心 pin 都保持它，旧实现回中每事件转视线
    // 与 pin 互相拉扯）。松手后 update() 沉降 θ；低空同序列松手后 θ 不动。
    setTiltedPose(*camera_, 3.0e6, 15.0);
    const double theta0 = offCenterAngle(*camera_);
    const double miss0 = viewLineMissDistance(*camera_);

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 20; ++i) {
        controller_->onPinchGesture(0.93f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
        // 手势期每个事件后 miss 距离都精确不变（相对 1e-9）。
        EXPECT_NEAR(1.0, viewLineMissDistance(*camera_) / miss0, 1e-9)
            << "event " << i;
    }
    controller_->onPinchEnd();
    for (int i = 0; i < 120; ++i) {
        controller_->update(1.0 / 60.0);
    }
    EXPECT_LT(offCenterAngle(*camera_), 0.1 * theta0);

    // 低空：同样的手势+松手 update 序列，miss 距离不得变化（miss 是 dolly
    // 与 pin 的严格不变量，只有回中会缩小它；θ=asin(miss/r) 会随拉远自然
    // 减小，不是判据）。
    setTiltedPose(*camera_, 3.0e5, 15.0);
    controller_->onPinchEnd();  // 重置手势状态（上一段的 pinching_ 已结束）
    const double missLow0 = viewLineMissDistance(*camera_);
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 5; ++i) {
        controller_->onPinchGesture(0.95f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    }
    controller_->onPinchEnd();
    for (int i = 0; i < 120; ++i) {
        controller_->update(1.0 / 60.0);
    }
    EXPECT_NEAR(1.0, viewLineMissDistance(*camera_) / missLow0, 1e-9);
}

TEST_F(CameraControllerTest, LowAltitudeZoomOutDoesNotRecenter) {
    // 低空(低于回中介入海拔)拉远：回中不得介入——视线到地心的垂距是 dolly 与
    // 锚点跟手的严格不变量，任何变化都只能来自回中，故用它做精确判据。
    setTiltedPose(*camera_, 3.0e5, 15.0);
    const double miss0 = viewLineMissDistance(*camera_);

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 5; ++i) {
        controller_->onPinchGesture(0.95f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    }
    controller_->onPinchEnd();

    EXPECT_LT(camera_->position().length() - kEarthRadiusMeters, 1.5e6);
    EXPECT_NEAR(1.0, viewLineMissDistance(*camera_) / miss0, 1e-6);
}

TEST_F(CameraControllerTest, SetNadirOrbitViewRebuildsExpectedPose) {
    // orbit 约定收归 CameraController:设"赤道 0°E 正上方 1000km、正北朝上"
    // 后,update() 的 orbit 重建应给出 eye 在 +X 轴上、看向地心、屏幕上北。
    const Vec3 target(kEarthRadiusMeters, 0.0, 0.0);
    const Vec3 up(1.0, 0.0, 0.0);
    constexpr double kHeight = 1.0e6;
    controller_->setNadirOrbitView(target, up, kHeight);
    controller_->update(0.016);

    const glm::dvec3 eye = camera_->position().raw();
    EXPECT_NEAR(kEarthRadiusMeters + kHeight, eye.x, 1.0);
    EXPECT_NEAR(0.0, eye.y, 1.0);
    EXPECT_NEAR(0.0, eye.z, 1.0);
    const glm::dvec3 dir = glm::normalize(camera_->direction().raw());
    EXPECT_NEAR(-1.0, dir.x, 1e-9);
    // 屏幕上方 = 局部正北 = +Z(赤道处)。
    const glm::dvec3 camUp = glm::normalize(camera_->up().raw());
    EXPECT_NEAR(1.0, camUp.z, 1e-9);
}

TEST_F(CameraControllerTest, PinchZoomFlickGlidesThenSettles) {
    // A2：捏合缩放松手后应沿视线朝锚点继续滑一小段（惯性），最终停住。
    placeOrbitIdentity(*camera_, 6.0);
    controller_->update(0.0);
    constexpr double cx = 400.0;
    constexpr double cy = 300.0;
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(cx, cy, 800.0, 600.0));

    double t = 1.0;
    controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f, t);
    t += 0.016;
    for (int i = 0; i < 4; ++i) {
        controller_->onPinchGesture(1.15f, cx, cy, 0.0f, 0.0f, 0.0f, t);
        t += 0.016;
    }
    const double distRelease = camera_->position().distanceTo(anchor);
    controller_->onPinchEnd();

    controller_->update(1.0 / 60.0);
    const double distGlide = camera_->position().distanceTo(anchor);
    EXPECT_LT(distGlide, distRelease);  // 松手后继续拉近

    for (int i = 0; i < 600; ++i) controller_->update(1.0 / 60.0);
    const double distSettled = camera_->position().distanceTo(anchor);
    controller_->update(1.0 / 60.0);
    EXPECT_NEAR(distSettled, camera_->position().distanceTo(anchor), 1.0);
}

TEST_F(CameraControllerTest, ZoomInertiaBoundedNoFling) {
    // 反发散守卫（对应历史上 touchInertia 的 36× fling）：猛烈 flick 后跑大量帧，
    // 必须收敛、永不穿透高度地板、永不越过锚点飞出。对数距离建模从数学上保证。
    placeOrbitIdentity(*camera_, 6.0);
    controller_->update(0.0);
    constexpr double cx = 400.0;
    constexpr double cy = 300.0;

    double t = 1.0;
    controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f, t);
    t += 0.016;
    for (int i = 0; i < 8; ++i) {
        controller_->onPinchGesture(1.3f, cx, cy, 0.0f, 0.0f, 0.0f, t);
        t += 0.016;
    }
    controller_->onPinchEnd();
    for (int i = 0; i < 5000; ++i) controller_->update(1.0 / 60.0);

    const double alt =
        Ellipsoid::WGS84().cartesianToCartographic(camera_->position()).height();
    EXPECT_GE(alt, 49.0);                            // 未穿透 50m 地板
    EXPECT_LT(alt, 6.5 * kEarthRadiusMeters);        // 未发散
}

TEST_F(CameraControllerTest, ZoomInertiaKeepsAnchorPinnedDuringGlide) {
    // 滑行期沿 eye→anchor 直线 dolly，故 off-center 锚点全程钉在屏幕原位。
    placeOrbitIdentity(*camera_, 4.0);
    controller_->update(0.0);
    constexpr double ax = 500.0;
    constexpr double ay = 240.0;
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(ax, ay, 800.0, 600.0));

    double t = 1.0;
    controller_->onPinchGesture(1.0f, ax, ay, 0.0f, 0.0f, 0.0f, t);
    t += 0.016;
    for (int i = 0; i < 4; ++i) {
        controller_->onPinchGesture(1.15f, ax, ay, 0.0f, 0.0f, 0.0f, t);
        t += 0.016;
    }
    controller_->onPinchEnd();

    for (int i = 0; i < 8; ++i) {
        controller_->update(1.0 / 60.0);
        glm::dvec2 p = projectToScreen(*camera_, anchor);
        EXPECT_NEAR(ax, p.x, 2.0);
        EXPECT_NEAR(ay, p.y, 2.0);
    }
}

TEST_F(CameraControllerTest, PinchPanFlickGlidesThenDecays) {
    // 步骤6：双指刚性 pan 甩动松手后沿运动方向继续滑行（与单指拖拽共用
    // 惯性通道），随后指数衰减停住。
    placeOrbitIdentity(*camera_, 3.0);
    controller_->update(0.0);

    double t = 1.0;
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Manipulate, t));
    float x = 400.0f;
    for (int i = 0; i < 6; ++i) {
        t += 0.016;
        x += 12.0f;
        controller_->onPinchGesture(pinchIn(
            1.0f, 0.0f, x, 300.0f, CameraController::PinchMode::Manipulate, t));
    }
    controller_->onPinchEnd();

    const auto qEnd = controller_->rotation();
    controller_->update(1.0 / 60.0);
    const auto qGlide = controller_->rotation();
    const double glideDiff =
        std::abs(qEnd.w - qGlide.w) + std::abs(qEnd.x - qGlide.x) +
        std::abs(qEnd.y - qGlide.y) + std::abs(qEnd.z - qGlide.z);
    EXPECT_GT(glideDiff, 1e-9);  // 松手后仍在转

    for (int i = 0; i < 240; ++i) controller_->update(1.0 / 60.0);
    const auto qBefore = controller_->rotation();
    controller_->update(1.0 / 60.0);
    const auto qAfter = controller_->rotation();
    const double settledDiff =
        std::abs(qBefore.w - qAfter.w) + std::abs(qBefore.x - qAfter.x) +
        std::abs(qBefore.y - qAfter.y) + std::abs(qBefore.z - qAfter.z);
    EXPECT_LT(settledDiff, 1e-6);  // 已衰减停住
}

TEST_F(CameraControllerTest, PinchPanAndZoomDoubleInertiaConvergesToWorldAnchor) {
    // 步骤6：pan+zoom 双惯性并行。zoomInertiaAnchor_ 是固定世界点，pan 惯性
    // 转的是相机（世界点不动），滑行期 dolly 持续朝原世界锚点收敛——距离
    // 单调减、旋转持续、全程有界不发散。
    placeOrbitIdentity(*camera_, 4.0);
    controller_->update(0.0);
    const Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    double t = 1.0;
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Manipulate, t));
    for (int i = 1; i <= 6; ++i) {
        t += 0.016;
        controller_->onPinchGesture(pinchIn(
            1.0f + 0.04f * static_cast<float>(i), 0.0f,
            400.0f + 12.0f * static_cast<float>(i), 300.0f,
            CameraController::PinchMode::Manipulate, t));
    }
    controller_->onPinchEnd();

    double prevDist = camera_->position().distanceTo(anchor);
    const auto q0 = controller_->rotation();
    for (int i = 0; i < 8; ++i) {
        controller_->update(1.0 / 60.0);
        const double dist = camera_->position().distanceTo(anchor);
        EXPECT_LT(dist, prevDist) << "frame " << i;  // zoom 持续朝世界锚点
        prevDist = dist;
    }
    const auto q1 = controller_->rotation();
    const double rotDiff = std::abs(q0.w - q1.w) + std::abs(q0.x - q1.x) +
                           std::abs(q0.y - q1.y) + std::abs(q0.z - q1.z);
    EXPECT_GT(rotDiff, 1e-9);  // pan 同时在滑

    for (int i = 0; i < 1000; ++i) controller_->update(1.0 / 60.0);
    const double alt =
        Ellipsoid::WGS84().cartesianToCartographic(camera_->position()).height();
    EXPECT_GE(alt, 49.0);
    EXPECT_LT(alt, 6.5 * kEarthRadiusMeters);  // 不发散
}

TEST_F(CameraControllerTest, PinchPitchReleaseDoesNotSeedPanInertia) {
    // Pitch 模式锚点钉 latch 像素，pin 增量恒为 identity → 松手不得产生
    // pan 滑行（旧架构里竖向质心移动会被当 pan 积速度）。
    placeOrbitIdentity(*camera_, 4.0);
    controller_->update(0.0);

    double t = 1.0;
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Undecided, t));
    t += 0.016;
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Pitch, t));
    for (int i = 1; i <= 6; ++i) {
        t += 0.016;
        controller_->onPinchGesture(pinchIn(
            1.0f, 0.0f, 400.0f, 300.0f - 25.0f * static_cast<float>(i),
            CameraController::PinchMode::Pitch, t));
    }
    controller_->onPinchEnd();

    const auto qEnd = controller_->rotation();
    for (int i = 0; i < 5; ++i) controller_->update(1.0 / 60.0);
    const auto qAfter = controller_->rotation();
    const double diff =
        std::abs(qEnd.w - qAfter.w) + std::abs(qEnd.x - qAfter.x) +
        std::abs(qEnd.y - qAfter.y) + std::abs(qEnd.z - qAfter.z);
    EXPECT_LT(diff, 1e-9);
}

TEST_F(CameraControllerTest, DragInterruptsZoomInertia) {
    placeOrbitIdentity(*camera_, 6.0);
    controller_->update(0.0);
    constexpr double cx = 400.0;
    constexpr double cy = 300.0;
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(cx, cy, 800.0, 600.0));

    double t = 1.0;
    controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f, t);
    t += 0.016;
    for (int i = 0; i < 4; ++i) {
        controller_->onPinchGesture(1.15f, cx, cy, 0.0f, 0.0f, 0.0f, t);
        t += 0.016;
    }
    controller_->onPinchEnd();
    controller_->update(1.0 / 60.0);  // gliding

    controller_->onDragStart(cx, cy);
    controller_->onDragEnd();
    const double d0 = camera_->position().distanceTo(anchor);
    controller_->update(1.0 / 60.0);
    EXPECT_NEAR(d0, camera_->position().distanceTo(anchor), 0.5);  // 滑行已停
}

TEST_F(CameraControllerTest, PinchRotationChangesCameraAroundAnchor) {
    placeOrbitIdentity(*camera_, 7.0);
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
    placeOrbitIdentity(*camera_, 5.0);
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

TEST_F(CameraControllerTest, PinchUsesInjectedSurfacePickerForAnchor) {
    placeOrbitIdentity(*camera_, 5.0);
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

    // 锚点起手拾取一次、整段手势缓存复用——这是"锁点只拾一次"的规格化断言
    // （逐帧重 pick 会让指下地形高变化时锚点漂移）。
    EXPECT_EQ(pickCount, 1);
    EXPECT_LT(controller_->distance(), 5.0f);
}

TEST_F(CameraControllerTest, PinchAcquiresAnchorMidGestureAfterStartMiss) {
    // 起手 pick 全 miss（如从球外/天空起手）→ 无锚分支照样缩放；后续事件
    // 每次重试获取锚点，质心移回可拾取区后转入有锚分支。
    placeOrbitIdentity(*camera_, 5.0);
    controller_->update(0.0);

    int pickCount = 0;
    controller_->setSurfacePicker([&](float x, float y, Vec3& outPoint) {
        ++pickCount;
        if (x > 600.0f) {
            return false;  // 球外区域：起手 miss
        }
        Ray ray = camera_->getPickRay(x, y, 800.0, 600.0);
        outPoint = intersectEarthSphere(ray);
        return true;
    });

    const float before = controller_->distance();
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 700.0f, 100.0f, CameraController::PinchMode::Manipulate));
    controller_->onPinchGesture(pinchIn(
        1.2f, 0.0f, 700.0f, 100.0f, CameraController::PinchMode::Manipulate));
    Vec3 anchorWorld;
    EXPECT_FALSE(controller_->debugAnchorWorld(anchorWorld));
    EXPECT_LT(controller_->distance(), before);  // 无锚也在缩放

    controller_->onPinchGesture(pinchIn(
        1.3f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Manipulate));
    EXPECT_TRUE(controller_->debugAnchorWorld(anchorWorld));  // 重试已获取
    EXPECT_GE(pickCount, 2);
    controller_->onPinchEnd();
}

TEST_F(CameraControllerTest, PinchZoomClampsToMinAltitudeInsteadOfStopping) {
    placeOrbitIdentity(*camera_, 1.01);
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

TEST_F(CameraControllerTest, HighAltitudePinchSkipsTerrainHeightQuery) {
    // Perf regression: the terrain-height clamp scans every loaded tile's mesh
    // triangles, so it must NOT run when the eye is far above any terrain
    // (Everest ~8.8 km). Otherwise panning/zooming at altitude costs >150 ms/fr.
    int terrainCalls = 0;
    controller_->setTerrainHeightFunc([&](const Vec3&) -> double {
        ++terrainCalls;
        return 0.0;
    });
    placeOrbitIdentity(*camera_, 1.02);  // ~127 km altitude — well above terrain
    controller_->update(0.0);

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 5; ++i) {
        controller_->onPinchGesture(1.1f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    }
    controller_->update(0.016);

    EXPECT_EQ(0, terrainCalls);
}

TEST_F(CameraControllerTest, TerrainClampHoldsLastKnownHeightWhenSampleMissing) {
    // No-data regression: when the terrain sampler returns nullopt (tile not yet
    // loaded / no coverage), the altitude clamp must hold the last known terrain
    // height rather than treat the point as sea level (which would let the eye
    // sink to ~50 m over not-yet-loaded high terrain, then pop back up on load).
    std::optional<double> sampled = 3000.0;  // valid high terrain
    controller_->setTerrainHeightFunc(
        [&](const Vec3&) -> std::optional<double> { return sampled; });
    placeOrbitIdentity(*camera_, 1.001);  // ~6.4 km, below the 9 km fast-path
    controller_->update(0.0);

    // Zoom in hard so the eye pins against the terrain floor (~3000 + 50 m) and
    // the valid sample populates the last-known cache.
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 24; ++i) {
        controller_->onPinchGesture(1.3f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    }
    controller_->update(0.0);
    const double altitudeWithData =
        Ellipsoid::WGS84().cartesianToCartographic(camera_->position()).height();
    EXPECT_GE(altitudeWithData, 3000.0);

    // Terrain data now unavailable → clamp must hold the ~3000 m floor, NOT drop
    // to sea level + 50 m.
    sampled = std::nullopt;
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 24; ++i) {
        controller_->onPinchGesture(1.3f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    }
    controller_->update(0.0);
    const double altitudeNoData =
        Ellipsoid::WGS84().cartesianToCartographic(camera_->position()).height();
    EXPECT_GE(altitudeNoData, 3000.0);
}

TEST_F(CameraControllerTest, DataDrivenTerrainRiseClampsImmediately) {
    // 刚体优先：新瓦片加载证明脚下是山（数据驱动的地形上升）必须单帧生效，
    // 不许像 Cesium 对称 10% 滤波那样延迟 ~0.5s（那 0.5s 里相机在山体内）。
    std::optional<double> sampled = 0.0;
    controller_->setTerrainHeightFunc(
        [&](const Vec3&) -> std::optional<double> { return sampled; });
    const auto& e = Ellipsoid::WGS84();
    const Vec3 target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    controller_->viewDistance(target, 500.0);  // 自由模式，低空
    controller_->update(0.016);

    sampled = 3000.0;  // 瓦片加载：脚下其实是 3000 m 山体
    controller_->update(0.016);  // 无用户输入 → 数据驱动路径

    EXPECT_GE(e.cartesianToCartographic(camera_->position()).height(), 3049.0);
    EXPECT_NEAR(3000.0, controller_->groundState().terrainHeightMeters, 1e-9);
}

TEST_F(CameraControllerTest, DataDrivenTerrainDropDecaysExponentially) {
    // 数据驱动的大幅下降走 τ=0.5s 指数逼近：钳位只抬不压，相机不受影响，
    // 平滑的是下游（动态 near）消费的地面高度——防 LOD 粗细瓦片交替时
    // near 逐帧跳。
    std::optional<double> sampled = 3000.0;
    controller_->setTerrainHeightFunc(
        [&](const Vec3&) -> std::optional<double> { return sampled; });
    const auto& e = Ellipsoid::WGS84();
    const Vec3 target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    controller_->viewDistance(target, 500.0);
    controller_->update(0.016);  // 建立滤波基线 3000
    ASSERT_NEAR(3000.0, controller_->groundState().terrainHeightMeters, 1e-9);

    sampled = 0.0;  // 粗瓦片替换：样本骤降 3000 → 0（数据驱动）
    controller_->update(0.016);
    const double afterOneFrame = controller_->groundState().terrainHeightMeters;
    EXPECT_GT(afterOneFrame, 2700.0);   // 单帧下降 < 300 m
    EXPECT_LT(afterOneFrame, 3000.0);   // 但确实在收敛

    for (int i = 0; i < 120; ++i) {     // ~1.9 s ≈ 4τ 后应基本收敛
        controller_->update(0.016);
    }
    EXPECT_LT(controller_->groundState().terrainHeightMeters, 100.0);
}

TEST_F(CameraControllerTest, UserDrivenTerrainChangeBypassesFilter) {
    // 用户驱动（手势事件）一律立即取用原始样本——滤波只作用于相机静止时
    // 的数据变化。
    std::optional<double> sampled = 3000.0;
    controller_->setTerrainHeightFunc(
        [&](const Vec3&) -> std::optional<double> { return sampled; });
    const auto& e = Ellipsoid::WGS84();
    const Vec3 target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    controller_->viewDistance(target, 500.0);
    controller_->update(0.016);
    ASSERT_NEAR(3000.0, controller_->groundState().terrainHeightMeters, 1e-9);

    sampled = 0.0;
    // 双指缩放事件（user-driven）：滤波必须被旁路，样本立即生效。
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.05f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchEnd();
    EXPECT_NEAR(0.0, controller_->groundState().terrainHeightMeters, 1e-9);
}

TEST_F(CameraControllerTest, SweptClampCatchesRidgeCrossedInOneFrame) {
    // 扫掠碰撞:单帧大位移跨过山脊时,静态单点钳位两端都在低地、直接隧穿;
    // 探针的扫掠走廊沿位移线段补采样,必须把山脊计入碰撞口径,把相机抬到
    // 脊高之上。
    const auto& e = Ellipsoid::WGS84();
    const double lon0 = glm::radians(106.5);
    const double lat0 = glm::radians(29.6);
    // 山脊:南北走向,位于起点以西 1000m,半宽 300m,高 3000m。
    const double lonRidge = lon0 - 1000.0 / (kEarthRadiusMeters *
                                             std::cos(lat0));
    controller_->setTerrainAreaSampleFunc(makeAnalyticAreaSampler(
        [&](double lon, double lat) {
            const double d = (lon - lonRidge) * kEarthRadiusMeters *
                std::cos(lat);
            return std::abs(d) < 300.0 ? 3000.0 : 0.0;
        }));

    const Vec3 target = e.cartographicToCartesian(
        Cartographic(lon0, lat0, 0.0));
    controller_->viewDistance(target, 500.0);  // 自由模式,AGL~500 平地
    controller_->update(0.016);
    ASSERT_LT(e.cartesianToCartographic(camera_->position()).height(),
              1500.0);

    // 单帧 2km 位移跨过山脊(外部裸写,如惯性合并/快速平移的极端事件)。
    const Cartographic startCart =
        e.cartesianToCartographic(camera_->position());
    const Vec3 landedEye = e.cartographicToCartesian(Cartographic(
        startCart.longitude() - 2000.0 / (kEarthRadiusMeters *
                                          std::cos(startCart.latitude())),
        startCart.latitude(),
        startCart.height()));
    camera_->lookAt(landedEye, target, camera_->up());

    controller_->update(0.016);
    EXPECT_GE(e.cartesianToCartographic(camera_->position()).height(),
              3049.0);
}

TEST_F(CameraControllerTest, AreaProbeRebuildsAtMostOncePerFrame) {
    // 成本约束:手势期高频事件(120/s)共享同帧探针,区域采样每帧至多 1 次。
    int callCount = 0;
    controller_->setTerrainAreaSampleFunc(makeAnalyticAreaSampler(
        [](double, double) { return 0.0; }, &callCount));
    const auto& e = Ellipsoid::WGS84();
    const Vec3 target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    controller_->viewDistance(target, 500.0);
    controller_->update(0.016);

    callCount = 0;
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Manipulate));
    for (int i = 1; i <= 60; ++i) {
        controller_->onPinchGesture(pinchIn(
            1.0f + 0.001f * i, 0.0f, 400.0f + i, 300.0f,
            CameraController::PinchMode::Manipulate,
            0.008 * i));
    }
    controller_->onPinchEnd();
    EXPECT_LE(callCount, 1);
}

TEST_F(CameraControllerTest, PinchPanIntoMountainKeepsAnchorPinned) {
    // §保锚论证的机器化判据:双指刚性平移把相机推进高地形区,碰撞解算沿
    // eye→anchor 直线退出——锚点像素必须每一步严格钉在质心下,同时 AGL
    // 不低于地形+50m。径向抬升(旧实现)在这里会泄漏 anchorErr。
    const auto& e = Ellipsoid::WGS84();
    const double lon0 = glm::radians(106.5);
    const double lat0 = glm::radians(29.6);
    // 高原:起点 1.2km 以外全部 4000m(平移足够远必然进入)。
    controller_->setTerrainAreaSampleFunc(makeAnalyticAreaSampler(
        [&](double lon, double lat) {
            const double dx = (lon - lon0) * kEarthRadiusMeters *
                std::cos(lat0);
            const double dy = (lat - lat0) * kEarthRadiusMeters;
            return std::sqrt(dx * dx + dy * dy) > 1200.0 ? 4000.0 : 0.0;
        }));
    const Vec3 target = e.cartographicToCartesian(
        Cartographic(lon0, lat0, 0.0));
    controller_->viewDistance(target, 2500.0);
    controller_->update(0.016);

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Manipulate));
    Vec3 anchorWorld;
    ASSERT_TRUE(controller_->debugAnchorWorld(anchorWorld));

    double maxAnchorErr = 0.0;
    for (int i = 1; i <= 120; ++i) {
        const float cx = 400.0f + 3.0f * i;  // 长路径横向平移
        controller_->onPinchGesture(pinchIn(
            1.0f, 0.0f, cx, 300.0f,
            CameraController::PinchMode::Manipulate, 0.008 * i));
        ASSERT_TRUE(controller_->debugAnchorWorld(anchorWorld));
        const glm::dvec2 px = projectToScreen(*camera_, anchorWorld);
        maxAnchorErr = std::max(maxAnchorErr,
                                std::abs(px.x - static_cast<double>(cx)));
        maxAnchorErr = std::max(maxAnchorErr, std::abs(px.y - 300.0));
        // 全程不穿地:AGL ≥ 地形+50(容差 1m)。
        const double h =
            e.cartesianToCartographic(camera_->position()).height();
        EXPECT_GE(h, std::max(controller_->groundState().terrainHeightMeters,
                              0.0) + 49.0)
            << "step " << i;
    }
    controller_->onPinchEnd();
    EXPECT_LT(maxAnchorErr, 1e-6);
    // 确认确实驶入了高地形区(测试非空转)。
    EXPECT_NEAR(4000.0, controller_->groundState().terrainHeightMeters, 1e-9);
}

TEST_F(CameraControllerTest, FrameEndSentinelClampsExternalBareLookAt) {
    // Facade/JNI 可绕过控制器直接写 Camera（resetCamera/nativeGrazingView）。
    // 帧末哨兵必须在下一次 update() 把非法位姿收编钳出地面——调用方无需
    // 自带钳位。
    const auto& e = Ellipsoid::WGS84();
    const Vec3 target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, 0.0));
    controller_->viewDistance(target, 5000.0);  // 进自由模式

    const Vec3 undergroundEye = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, -5000.0));
    camera_->lookAt(undergroundEye, Vec3::zero(), Vec3::unitZ());
    EXPECT_LT(e.cartesianToCartographic(camera_->position()).height(), 0.0);

    controller_->update(0.016);
    EXPECT_GE(e.cartesianToCartographic(camera_->position()).height(), 49.0);
}

TEST_F(CameraControllerTest, OrbitRebuildDoesNotOscillateAgainstClamp) {
    // orbit 位姿每帧由 rotation_/distance_ 重建：约束必须表达在 distance_ 上，
    // 直接抬 eye 会被下一帧重建抹掉 → 逐帧 dip→pop 振荡。高地形上连续跑
    // 60 帧，位姿必须收敛后逐帧稳定。
    controller_->setTerrainHeightFunc(
        [](const Vec3&) -> std::optional<double> { return 5000.0; });
    const auto& e = Ellipsoid::WGS84();
    const Vec3 target =
        e.cartographicToCartesian(Cartographic::fromDegrees(0.0, 0.0, 0.0));
    controller_->setNadirOrbitView(
        target, e.geodeticSurfaceNormal(target), 100.0);

    Vec3 prevEye;
    for (int i = 0; i < 60; ++i) {
        controller_->update(0.016);
        const Vec3 eye = camera_->position();
        if (i >= 2) {
            // 首两帧允许收敛位移，其后必须逐帧稳定（无钳位↔重建拉锯）。
            EXPECT_LT((eye - prevEye).length(), 1e-3) << "frame " << i;
        }
        prevEye = eye;
    }
    EXPECT_GE(e.cartesianToCartographic(camera_->position()).height(), 5049.0);
}

TEST_F(CameraControllerTest, MeasurementFreezeSentinelIsObserveOnly) {
    // 冻结契约：update() 不碰相机、位姿逐帧字节稳定——帧末哨兵在冻结期
    // 必须 observeOnly，即使位姿非法（地下）也不修。
    const auto& e = Ellipsoid::WGS84();
    controller_->setMeasurementFreeze(true);
    const Vec3 undergroundEye = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, -5000.0));
    camera_->lookAt(undergroundEye, Vec3::zero(), Vec3::unitZ());

    const Vec3 posBefore = camera_->position();
    const Vec3 dirBefore = camera_->direction();
    for (int i = 0; i < 10; ++i) {
        controller_->update(0.016);
    }
    EXPECT_EQ(posBefore, camera_->position());
    EXPECT_EQ(dirBefore, camera_->direction());
}

TEST_F(CameraControllerTest, PinchRotationSignIsPredictable) {
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);
    Vec3 reference = intersectEarthSphere(
        camera_->getPickRay(400.0, 250.0, 800.0, 600.0));
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.25f, 0.0f, 0.0f);
    controller_->update(0.0);
    const glm::dvec2 positiveProjected = projectToScreen(*camera_, reference);

    placeOrbitIdentity(*camera_, 7.0);
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
    placeOrbitIdentity(*camera_, 7.0);
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

TEST_F(CameraControllerTest, PinchTiltKeepsAnchorScreenPositionStable) {
    // Pitch 模式：绕锚点 pitch、锚点钉在 latch 像素原地不动（结构性保证，
    // 旧渐近混合实现下漂 ~17px，容差从 3.0 收紧到 0.5）。
    placeOrbitIdentity(*camera_, 4.0);
    controller_->update(0.0);

    constexpr float cx = 400.0f;
    constexpr float cy = 300.0f;
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(cx, cy, 800.0, 600.0));

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, cx, cy, CameraController::PinchMode::Undecided));
    // latch 迁移事件（锚点钉合像素与 pitch 基线取此刻质心）。
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, cx, cy, CameraController::PinchMode::Pitch));
    // 纯倾斜：质心竖向移动（绝对量），无缩放/旋转。多帧累积放大漂移。
    for (int i = 1; i <= 6; ++i) {
        controller_->onPinchGesture(pinchIn(
            1.0f, 0.0f, cx, cy - 20.0f * static_cast<float>(i),
            CameraController::PinchMode::Pitch));
    }
    controller_->update(0.0);

    glm::dvec2 projected = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(cx, projected.x, 0.5);
    EXPECT_NEAR(cy, projected.y, 0.5);
}

TEST_F(CameraControllerTest, PinchPushUpIncreasesTiltPullDownDecreasesTilt) {
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);
    constexpr float cx = 400.0f;
    constexpr float cy = 300.0f;
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, cx, cy, CameraController::PinchMode::Undecided));
    // latch 迁移事件（基线取此刻质心）。
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, cx, cy, CameraController::PinchMode::Pitch));

    // 先建立一个明显的斜视倾角，脱离 nadir 退化区（nadir 处已无更多下倾空间，
    // pitch 轴/up 方向病态，倾斜方向断言无意义）。质心 Y 绝对映射：向上推。
    for (int i = 1; i <= 8; ++i) {
        controller_->onPinchGesture(pinchIn(
            1.0f, 0.0f, cx, cy - 40.0f * static_cast<float>(i),
            CameraController::PinchMode::Pitch));
    }
    controller_->update(0.0);
    const double beforePushUpSlope = cameraSlope(*camera_);

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, cx, cy - 40.0f * 9.0f,
        CameraController::PinchMode::Pitch));
    controller_->update(0.0);
    const double afterPushUpSlope = cameraSlope(*camera_);
    EXPECT_LT(afterPushUpSlope, beforePushUpSlope);

    const double beforePullDownSlope = afterPushUpSlope;
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, cx, cy - 40.0f * 8.0f,
        CameraController::PinchMode::Pitch));
    controller_->update(0.0);
    const double afterPullDownSlope = cameraSlope(*camera_);
    EXPECT_GT(afterPullDownSlope, beforePullDownSlope);
}

TEST_F(CameraControllerTest, PinchHorizontalPanMovesAnchorWithCentroid) {
    // N1（反转旧 PinchHorizontalPanDoesNotMoveCamera，其期望本身是旧架构的
    // 产物）：双指质心平移 = 刚性 pan，锚点钉在移动质心下（Google Maps 手感）。
    placeOrbitIdentity(*camera_, 5.0);
    controller_->update(0.0);
    const auto before = camera_->viewMatrix();
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Manipulate));
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 430.0f, 300.0f, CameraController::PinchMode::Manipulate));
    controller_->update(0.0);

    EXPECT_GT(matrixAbsDiff(before, camera_->viewMatrix()), 1e-6);  // 相机动了
    const glm::dvec2 projected = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(430.0, projected.x, 0.5);
    EXPECT_NEAR(300.0, projected.y, 0.5);
}

TEST_F(CameraControllerTest, PinchRigidPanAnchorErrStaysZeroOverLongPath) {
    // N2：长路径刚性判定——每一步锚点投影都精确等于当前质心（不是渐近逼近）。
    // 旧 0.12 渐近跟随下每步落后 88%，anchorErr 持续 >10px。
    placeOrbitIdentity(*camera_, 3.0);
    controller_->update(0.0);
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Manipulate));
    float x = 400.0f;
    for (int i = 0; i < 14; ++i) {
        x += 10.0f;
        controller_->onPinchGesture(pinchIn(
            1.0f, 0.0f, x, 300.0f, CameraController::PinchMode::Manipulate));
        const glm::dvec2 p = projectToScreen(*camera_, anchor);
        EXPECT_NEAR(x, p.x, 0.5) << "step " << i;
        EXPECT_NEAR(300.0, p.y, 0.5) << "step " << i;
    }
    controller_->onPinchEnd();
}

TEST_F(CameraControllerTest, PinchCombinedZoomTwistPanAllPinned) {
    // N3：三通道同帧并行（缩放+拧动+质心移动），锚点始终钉在当前质心。
    // 覆盖旧 PinchScaleDominanceSuppressesTilt 的实质内容。
    placeOrbitIdentity(*camera_, 5.0);
    controller_->update(0.0);
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Manipulate));
    for (int i = 1; i <= 4; ++i) {
        const float f = static_cast<float>(i);
        controller_->onPinchGesture(pinchIn(
            1.0f + 0.03f * f, 0.01f * f,
            400.0f + 10.0f * f, 300.0f - 6.0f * f,
            CameraController::PinchMode::Manipulate));
        const glm::dvec2 p = projectToScreen(*camera_, anchor);
        EXPECT_NEAR(400.0 + 10.0 * i, p.x, 0.5) << "step " << i;
        EXPECT_NEAR(300.0 - 6.0 * i, p.y, 0.5) << "step " << i;
    }
    controller_->onPinchEnd();
}

TEST_F(CameraControllerTest, PinchDiagonalCenterMoveIsPurePan) {
    // 语义升级（原 PinchDiagonalCenterMoveDoesNotTilt）：斜向质心移动 =
    // 纯刚性 pan——锚点钉到新质心，且俯仰严格不变（绕地心刚性旋转精确保持
    // slope，任何变化都说明混入了 tilt/dolly）。
    placeOrbitIdentity(*camera_, 5.0);
    controller_->update(0.0);
    const double slopeBefore = cameraSlope(*camera_);
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Manipulate));
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 430.0f, 275.0f, CameraController::PinchMode::Manipulate));
    controller_->update(0.0);

    const glm::dvec2 p = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(430.0, p.x, 0.5);
    EXPECT_NEAR(275.0, p.y, 0.5);
    EXPECT_NEAR(slopeBefore, cameraSlope(*camera_), 1e-9);
}

TEST_F(CameraControllerTest, PinchLatchWindowCatchUpLosesNoPan) {
    // N6：Undecided 窗口锚点钉起手质心；latch 成 Manipulate 的一刻，pin 是
    // 状态求解而非增量累加——窗口期积压的质心行程被一次精确补齐，不丢量。
    placeOrbitIdentity(*camera_, 5.0);
    controller_->update(0.0);
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Undecided));
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 405.0f, 300.0f, CameraController::PinchMode::Undecided));
    // 窗口期锚点仍钉在起手质心。
    glm::dvec2 p = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(400.0, p.x, 0.5);

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 408.0f, 300.0f, CameraController::PinchMode::Manipulate));
    p = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(408.0, p.x, 0.5);  // 一次精确追平
    EXPECT_NEAR(300.0, p.y, 0.5);
    controller_->onPinchEnd();
}

TEST_F(CameraControllerTest, PinchPitchModeIgnoresHorizontalCentroidDrift) {
    // N7：Pitch 模式下质心横向漂移不产生 pan——锚点钉在 latch 像素。
    placeOrbitIdentity(*camera_, 4.0);
    controller_->update(0.0);
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));
    const double slopeBefore = cameraSlope(*camera_);

    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Undecided));
    controller_->onPinchGesture(pinchIn(
        1.0f, 0.0f, 400.0f, 300.0f, CameraController::PinchMode::Pitch));
    for (int i = 1; i <= 5; ++i) {
        controller_->onPinchGesture(pinchIn(
            1.0f, 0.0f,
            400.0f + 15.0f * static_cast<float>(i),   // 横向漂移
            300.0f - 25.0f * static_cast<float>(i),   // 竖向驱动 pitch
            CameraController::PinchMode::Pitch));
    }
    controller_->update(0.0);

    const glm::dvec2 p = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(400.0, p.x, 0.5);
    EXPECT_NEAR(300.0, p.y, 0.5);
    // pitch 确实发生。量级说明：nadir 起步、绕 3R 外锚点转 θ 时 slope 角只变
    // ~θ/4（eye 沿锚点弧移动同时带动本地竖直方向），θ=0.1875 → Δslope≈1.1e-3。
    EXPECT_LT(cameraSlope(*camera_), slopeBefore - 5e-4);
}

TEST_F(CameraControllerTest, PinchTiltGainIsViewportIndependent) {
    // N11：pitch 增益按视口高度归一——同一"占屏比例"的竖移在不同分辨率下
    // 产生相同俯仰变化（旧的每物理像素定义在 3.5x 屏上比 1.5x 快 2.3 倍）。
    auto runTilt = [](int width, int height) {
        Camera camera;
        camera.setPerspective(glm::radians(60.0), 1.0, 50000000.0);
        CameraController controller(&camera);
        controller.setViewport(width, height);
        placeOrbitIdentity(camera, 4.0);
        controller.update(0.0);
        const float cx = static_cast<float>(width) * 0.5f;
        const float cy = static_cast<float>(height) * 0.5f;
        controller.onPinchGesture(pinchIn(
            1.0f, 0.0f, cx, cy, CameraController::PinchMode::Undecided));
        controller.onPinchGesture(pinchIn(
            1.0f, 0.0f, cx, cy, CameraController::PinchMode::Pitch));
        const float dy = static_cast<float>(height) * 0.25f;
        for (int i = 1; i <= 5; ++i) {
            controller.onPinchGesture(pinchIn(
                1.0f, 0.0f, cx,
                cy - dy * static_cast<float>(i) / 5.0f,
                CameraController::PinchMode::Pitch));
        }
        return camera.direction().dot(-camera.position().normalized());
    };
    EXPECT_NEAR(runTilt(800, 600), runTilt(1600, 1200), 1e-6);
}

TEST_F(CameraControllerTest, ViewDistanceMovesCameraTowardPickedTarget) {
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);

    Vec3 target = intersectEarthSphere(
        camera_->getPickRay(420.0, 280.0, 800.0, 600.0));
    const double requestedDistance = camera_->position().distanceTo(target) * 0.57;

    controller_->viewDistance(target, requestedDistance);

    EXPECT_NEAR(requestedDistance, camera_->position().distanceTo(target), 1e-3);
    EXPECT_GT(camera_->direction().dot((target - camera_->position()).normalized()), 0.999);
}

TEST_F(CameraControllerTest, ViewDistanceRespectsNearGroundSafetyFloor) {
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);

    Vec3 target = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->viewDistance(target, 10.0);

    EXPECT_NEAR(50.0, camera_->position().distanceTo(target), 1e-3);
    EXPECT_GT(camera_->direction().dot((target - camera_->position()).normalized()), 0.999);
}

TEST_F(CameraControllerTest, NadirViewPitchIsNegativeHalfPi) {
    // identity 轨道 → 相机正俯视地心，pitch 应为 -π/2。
    placeOrbitIdentity(*camera_, 7.0);
    controller_->update(0.0);
    const double pi = std::acos(-1.0);
    EXPECT_NEAR(-pi / 2.0, controller_->pitchRadians(), 0.02);
}

TEST_F(CameraControllerTest, ResetNorthUpZerosHeading) {
    placeOrbitIdentity(*camera_, 3.0);
    controller_->update(0.0);

    // 双指旋转(绕地心竖轴 twist)制造明显非零方位角。
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.4f, 0.0f, 0.0f);
    controller_->onPinchEnd();
    controller_->update(0.0);

    const double pi = std::acos(-1.0);
    double before = controller_->headingRadians();
    if (before > pi) before -= 2.0 * pi;
    ASSERT_GT(std::abs(before), 0.05);  // 确实偏离正北，测试才有意义

    controller_->resetNorthUp();

    double after = controller_->headingRadians();
    if (after > pi) after -= 2.0 * pi;
    EXPECT_NEAR(0.0, after, pi / 180.0);  // 复位到 ±1° 内
}

TEST_F(CameraControllerTest, ResetNorthUpPreservesPitch) {
    placeOrbitIdentity(*camera_, 3.0);
    controller_->update(0.0);

    // 先倾斜脱离正俯视，再 twist 制造方位角。
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 6; ++i) {
        controller_->onPinchGesture(1.0f, 400.0f,
            static_cast<float>(300 - 20), 0.0f, 0.0f, -20.0f);
    }
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.4f, 0.0f, 0.0f);
    controller_->onPinchEnd();
    controller_->update(0.0);

    const double pitchBefore = controller_->pitchRadians();
    controller_->resetNorthUp();
    const double pitchAfter = controller_->pitchRadians();

    EXPECT_NEAR(pitchBefore, pitchAfter, std::acos(-1.0) / 180.0);  // 俯仰保持
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

// 北极星测量台冻结相机：冻结后 update() 完全空转，即便存在待处理的甩动惯性，
// 相机位姿也逐帧字节稳定（far 重载耦合态可复现的机制基础）。
TEST_F(CameraControllerTest, MeasurementFreezeHoldsPoseDespiteInertia) {
    placeOrbitIdentity(*camera_, 1.0002);  // 近地表，接管地表拾取
    controller_->update(0.0);

    // 甩一下产生惯性：drag start→move→end 后 inertiaAngularVelocity_ > 0。
    controller_->onDragStart(400.0f, 300.0f, 0.0);
    controller_->onDragMove(500.0f, 300.0f, 1.0 / 60.0);
    controller_->onDragEnd();

    // 冻结：应立即清零惯性，之后任意步进都不动相机。
    controller_->setMeasurementFreeze(true);
    EXPECT_TRUE(controller_->measurementFrozen());

    const Mat4 frozenView = camera_->viewMatrix();
    for (int i = 0; i < 120; ++i) {
        controller_->update(1.0 / 60.0);
    }
    // 逐帧位姿不变（视图矩阵字节级稳定）。
    EXPECT_LT(matrixAbsDiff(camera_->viewMatrix(), frozenView), 1e-9);

    // 解冻后 update() 恢复正常（惯性已清零，故此处仅验证不再被锁死：
    // orbit 重建把相机放回 distance_ 对应的位置）。
    controller_->setMeasurementFreeze(false);
    EXPECT_FALSE(controller_->measurementFrozen());
    placeOrbitIdentity(*camera_, 2.0);
    controller_->update(1.0 / 60.0);
    EXPECT_GT(matrixAbsDiff(camera_->viewMatrix(), frozenView), 1e-6);
}

// 模拟 PickingService::pickTerrain 的返回形态：与球面求交后，把交点沿局部垂直
// 抬起地形高——返回点因此不在拾取射线上。真机上这段落差在斜视低空可达数百像素。
CameraController::SurfacePicker makeOffRayTerrainPicker(const Camera& camera,
                                                        double liftMeters) {
    return [&camera, liftMeters](float x, float y, Vec3& out) {
        const Ray ray = camera.getPickRay(x, y, 800.0, 600.0);
        const glm::dvec3 o = ray.origin().raw();
        const glm::dvec3 d = ray.direction().raw();
        const double b = 2.0 * glm::dot(o, d);
        const double c = glm::dot(o, o) - kEarthRadiusMeters * kEarthRadiusMeters;
        const double disc = b * b - 4.0 * c;
        if (disc < 0.0) return false;
        const double t0 = (-b - std::sqrt(disc)) * 0.5;
        const double t1 = (-b + std::sqrt(disc)) * 0.5;
        const double t = t0 > 0.0 ? t0 : t1;
        if (t <= 0.0) return false;
        const glm::dvec3 hit = ray.pointAt(t).raw();
        out = Vec3(hit + glm::normalize(hit) * liftMeters);
        return true;
    };
}

TEST_F(CameraControllerTest, DragStartDoesNotJumpWhenPickIsOffRay) {
    // 起手不跳：抓取之后手指原地不动的 move 必须不产生任何相机运动。
    // 锚点跟手的数学假定抓取点在起始射线上，而 pickTerrain 返回的点是沿局部
    // 垂直抬起的、不在射线上；旧实现把这段落差当作手指位移，在第一个 move
    // 一次性补掉——2026-08-01 真机实测起手锚点投影偏差达 227~471px。
    setTiltedPose(*camera_, 2.0e4, 45.0);
    controller_->setSurfacePicker(makeOffRayTerrainPicker(*camera_, 3000.0));

    const glm::dvec3 eyeBefore = camera_->position().raw();
    controller_->onDragStart(430.0f, 260.0f, 1.0);
    controller_->onDragMove(430.0f, 260.0f, 1.016);

    EXPECT_LT(glm::length(camera_->position().raw() - eyeBefore), 1.0);
}

TEST_F(CameraControllerTest, PinchKeepsVisibleSurfacePointUnderFingerWhenPickIsOffRay) {
    // 捏合路径的同一根因。判据取"用户真正看到的那个点"：双指质心像素射线上的
    // 地表点。旧实现把偏离射线的拾取点当锚点钉住，于是钉的是别的东西——手指
    // 下的地物反而被推开。scale 必须 ≠1，否则 zoomIntent 为假，
    // keepAnchorAtScreenPoint 根本不会被调用，测不到东西。
    constexpr double kLift = 3000.0;
    constexpr double cx = 430.0;
    constexpr double cy = 260.0;
    setTiltedPose(*camera_, 2.0e4, 45.0);
    controller_->setSurfacePicker(makeOffRayTerrainPicker(*camera_, kLift));

    // 手指下可见的地表点 = 拾取射线与半径 R+lift 球面的交点（fake picker 抬起
    // 后半径恒为 R+lift）。
    const Ray ray = camera_->getPickRay(cx, cy, 800.0, 600.0);
    const glm::dvec3 o = ray.origin().raw();
    const glm::dvec3 d = ray.direction().raw();
    const double radius = kEarthRadiusMeters + kLift;
    const double b = 2.0 * glm::dot(o, d);
    const double c = glm::dot(o, o) - radius * radius;
    const double disc = b * b - 4.0 * c;
    ASSERT_GE(disc, 0.0);
    const Vec3 visible = ray.pointAt((-b - std::sqrt(disc)) * 0.5);

    controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.2f, cx, cy, 0.0f, 0.0f, 0.0f);
    controller_->update(0.0);

    const glm::dvec2 projected = projectToScreen(*camera_, visible);
    EXPECT_NEAR(cx, projected.x, 2.0);
    EXPECT_NEAR(cy, projected.y, 2.0);
}
