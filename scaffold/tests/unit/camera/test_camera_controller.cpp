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

TEST_F(CameraControllerTest, DragKeepsGrabbedSurfacePointUnderFinger) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
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

    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
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

    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
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

    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(7.0f);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(7.0f);
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

TEST_F(CameraControllerTest, PinchCombinedZoomAndRotateKeepsAnchorPinned) {
    // A1 锚点锁：缩放+旋转同时发生时，手指下的地表点必须保持在双指中心。
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(5.0f);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(5.0f);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(5.0f);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(5.0f);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(3.0f);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(3.0f);
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
    controller_->onPinchEnd();

    const double thetaFinal = offCenterAngle(*camera_);
    EXPECT_LT(thetaFinal, 0.1 * theta0);
    EXPECT_LT(viewLineMissDistance(*camera_), 0.5 * miss0);

    // 球心投影落在屏幕中心附近。
    const glm::dvec2 centerPx = projectToScreen(*camera_, Vec3::zero());
    EXPECT_NEAR(400.0, centerPx.x, 30.0);
    EXPECT_NEAR(300.0, centerPx.y, 30.0);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(6.0f);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(6.0f);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(4.0f);
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

TEST_F(CameraControllerTest, DragInterruptsZoomInertia) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(6.0f);
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

TEST_F(CameraControllerTest, PinchUsesInjectedSurfacePickerForAnchor) {
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

TEST_F(CameraControllerTest, HighAltitudePinchSkipsTerrainHeightQuery) {
    // Perf regression: the terrain-height clamp scans every loaded tile's mesh
    // triangles, so it must NOT run when the eye is far above any terrain
    // (Everest ~8.8 km). Otherwise panning/zooming at altitude costs >150 ms/fr.
    int terrainCalls = 0;
    controller_->setTerrainHeightFunc([&](const Vec3&) -> double {
        ++terrainCalls;
        return 0.0;
    });
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(1.02f);  // ~127 km altitude — well above terrain
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(1.001f);  // ~6.4 km, below the 9 km fast-path
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

TEST_F(CameraControllerTest, PinchTiltKeepsAnchorScreenPositionStable) {
    // 真机观察到的缺陷：双指倾斜时地图被"平移"、锚点被推离原屏幕位。根因是
    // centerIntent 平移跟随把倾斜的竖向手指位移误当 pan，逐帧把锚点法线朝手指
    // 方向混合。倾斜应绕锚点 pitch、锚点原地不动。此处断言倾斜后原锚点仍投影
    // 在其起始屏幕位置附近。
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(4.0f);
    controller_->update(0.0);

    constexpr double cx = 400.0;
    constexpr double cy = 300.0;
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(cx, cy, 800.0, 600.0));

    controller_->onPinchGesture(1.0f, cx, cy, 0.0f, 0.0f, 0.0f);
    // 纯倾斜：双指中心竖向移动，无缩放/旋转。多帧累积放大漂移。
    for (int i = 0; i < 6; ++i) {
        controller_->onPinchGesture(
            1.0f, cx, static_cast<float>(cy - 20), 0.0f, 0.0f, -20.0f);
    }
    controller_->update(0.0);

    glm::dvec2 projected = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(cx, projected.x, 3.0);
    EXPECT_NEAR(cy, projected.y, 3.0);
}

TEST_F(CameraControllerTest, PinchPushUpIncreasesTiltPullDownDecreasesTilt) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);

    // 先建立一个明显的斜视倾角，脱离 nadir 退化区（nadir 处已无更多下倾空间，
    // pitch 轴/up 方向病态，倾斜方向断言无意义）。
    for (int i = 0; i < 8; ++i) {
        controller_->onPinchGesture(1.0f, 400.0f, 260.0f, 0.0f, 0.0f, -40.0f);
    }
    controller_->update(0.0);
    const double beforePushUpSlope = cameraSlope(*camera_);

    controller_->onPinchGesture(1.0f, 400.0f, 260.0f, 0.0f, 0.0f, -40.0f);
    controller_->update(0.0);
    const double afterPushUpSlope = cameraSlope(*camera_);
    EXPECT_LT(afterPushUpSlope, beforePushUpSlope);

    const double beforePullDownSlope = afterPushUpSlope;
    controller_->onPinchGesture(1.0f, 400.0f, 340.0f, 0.0f, 0.0f, 40.0f);
    controller_->update(0.0);
    const double afterPullDownSlope = cameraSlope(*camera_);
    EXPECT_GT(afterPullDownSlope, beforePullDownSlope);
}

TEST_F(CameraControllerTest, PinchHorizontalPanDoesNotMoveCamera) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    auto before = camera_->viewMatrix();

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 430.0f, 300.0f, 0.0f, 30.0f, 0.0f);
    controller_->update(0.0);

    auto after = camera_->viewMatrix();
    double diff = 0.0;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            diff += std::abs(before(i, j) - after(i, j));
        }
    }
    EXPECT_LT(diff, 1e-6);
}

TEST_F(CameraControllerTest, PinchDiagonalCenterMoveDoesNotTilt) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    auto before = camera_->viewMatrix();

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.0f, 430.0f, 275.0f, 0.0f, 30.0f, -25.0f);
    controller_->update(0.0);

    EXPECT_LT(matrixAbsDiff(before, camera_->viewMatrix()), 1e-6);
}

TEST_F(CameraControllerTest, PinchScaleDominanceSuppressesTilt) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(5.0f);
    controller_->update(0.0);
    Vec3 anchor = intersectEarthSphere(
        camera_->getPickRay(400.0, 300.0, 800.0, 600.0));

    controller_->onPinchGesture(1.0f, 400.0f, 300.0f, 0.0f, 0.0f, 0.0f);
    controller_->onPinchGesture(1.2f, 400.0f, 275.0f, 0.0f, 0.0f, -25.0f);
    controller_->update(0.0);

    glm::dvec2 projected = projectToScreen(*camera_, anchor);
    EXPECT_NEAR(400.0, projected.x, 2.0);
    EXPECT_NEAR(275.0, projected.y, 2.0);
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

TEST_F(CameraControllerTest, NadirViewPitchIsNegativeHalfPi) {
    // identity 轨道 → 相机正俯视地心，pitch 应为 -π/2。
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->update(0.0);
    const double pi = std::acos(-1.0);
    EXPECT_NEAR(-pi / 2.0, controller_->pitchRadians(), 0.02);
}

TEST_F(CameraControllerTest, ResetNorthUpZerosHeading) {
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(3.0f);
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
    controller_->setRotation(glm::dquat(1.0, 0.0, 0.0, 0.0));
    controller_->setDistance(3.0f);
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
    controller_->setDistance(1.0002f);  // 近地表，接管地表拾取
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
    controller_->setDistance(2.0f);
    controller_->update(1.0 / 60.0);
    EXPECT_GT(matrixAbsDiff(camera_->viewMatrix(), frozenView), 1e-6);
}
