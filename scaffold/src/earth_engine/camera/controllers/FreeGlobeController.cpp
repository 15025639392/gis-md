#include "FreeGlobeController.h"

#include "../CameraConstraintSolver.h"
#include "../CameraPoseOps.h"
#include "../../core/geodesy/Cartographic.h"
#include "../../core/geodesy/Ellipsoid.h"
#include "../../core/math/Mat4.h"
#include "../../core/math/Ray.h"
#include "../../debug/PlatformLog.h"
#include "../../scene/Camera.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

namespace earth_engine {

namespace {

constexpr double kMaxInertiaAngularVelocityRadPerSec = 5.0;
// iOS 列表手感（契约 1.4）：velocity *= 0.998 每毫秒 ≈ exp(-2.0/s)。
// 依据：UIScrollViewDecelerationRateNormal=0.998、Mapbox Maps iOS
// panDecelerationFactor 每毫秒乘一次。
constexpr double kInertiaDampingPerSecond = 2.0;
constexpr double kVelocitySmoothing = 0.35;
constexpr double kEarthRadiusMeters = 6378137.0;
// 相机包络常量（净空/最大地形高/地心距上限）归 CameraConstraintSolver 单点持有。
constexpr double kMinAltitudeMeters = CameraConstraintSolver::kMinClearanceMeters;
constexpr double kMaxTerrainHeightMeters =
    CameraConstraintSolver::kMaxTerrainHeightMeters;
constexpr double kMaxDistanceEarthRadii =
    CameraConstraintSolver::kMaxDistanceEarthRadii;
constexpr double kTouchJerkLimit = 0.3;
constexpr double kTouchMinSlope = 0.1;
// Pitch 增益：Mapbox 生产值 -0.5°/px（满倾角约 150px 竖移），保留质心
// 绝对值映射；真机标定项（契约 2.2）。
constexpr double kPinchTiltRadiansPerPixel = 0.008726646259971648;
constexpr double kPinchTiltMaxStepRadians = 0.08;
// 单指模式切换（契约 1.2）：海拔门限 = Cesium minimumPickingTerrainHeight
// (WGS84 生产默认)；倾斜门限 = MapLibre issue #6111 的高倾斜病态起点。
constexpr double kNearModeMaxAltitudeMeters = 150000.0;
constexpr double kNearModeMinPitchRadians = glm::pi<double>() / 3.0;
// 地平线裁剪（契约 1.3）：位移/惯性偏移不得超过地平线像素距离的 75%
// （MapLibre PR #6345；0.75 为手感调参，真机标定项）。
constexpr double kNearHorizonClampFactor = 0.75;
// 近地惯性触发下限：100px/s（Mapbox GL Native iOS 与 Flutter iOS 双重印证）。
constexpr double kNearMinInertiaVelocityPxPerSec = 100.0;
// 射线与锚点切平面近平行判据（数值守卫；主裁剪由地平线偏移量承担）。
constexpr double kNearPlaneGrazingEpsilon = 1e-4;
// 抓取球半径对 eye 半径的安全余量：锚点半径钳到 |eye|−margin 以下，防止
// 抓取球包住相机（见 tryAcquirePinchAnchor/grabSurfacePoint）。
constexpr double kGrabSphereEyeMarginMeters = 25.0;
// Jerk 限幅残差上限（对数距离空间，≈2.7×）。限幅只该把单事件的突跳摊到相邻
// 几个事件上，不该让长时间单向饱和攒出松手后仍在补的欠账。
constexpr double kMaxPinchScaleResidualLog = 1.0;

// 锚点求解病态区（掠射/球缘）的连续退化带：入射余弦 c=|dot(rayDir,法线)|
// 低于 hi 起把精确钉合旋转与转台旋转做 slerp 混合，低于 lo 完全转台。
// 精确解的像素→角度增益 ∝ 1/c，掠射时爆炸；而"命中就精确锚定、miss 就
// latch 转台"的硬切换（旧问题3）必然在切换点跳变或死锁。连续混合 + 退化区
// 整点重取锚点是唯一同时消掉两者的做法。
constexpr double kAnchorConditioningLo = 0.10;
constexpr double kAnchorConditioningHi = 0.35;

// 病态区混合权重：c ≥ hi 全精确（w=1），c ≤ lo 全转台（w=0），中间 smoothstep。
double anchorExactWeight(double conditioning) {
    const double t = std::clamp(
        (conditioning - kAnchorConditioningLo) /
            (kAnchorConditioningHi - kAnchorConditioningLo),
        0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Zoom 惯性：捏合松手后沿视线朝锚点继续滑一小段。刻意建模在"对数距离"空间
// （每秒的 ln(距离) 变化率），有三个好处：① 与高度无关，海拔 10km 和 100m
// 手感一致；② 指数逼近锚点、永不越过（distance*=exp(-r·dt)>0），从数学上排除
// 历史上那个 36× fling；③ 天然有界。速率上限 + 指数阻尼 + 低于地板即停。
constexpr double kZoomInertiaDampingPerSecond = 6.0;
constexpr double kMaxZoomInertiaLogRate = 6.0;   // |d(ln dist)/dt| 上限，滤抖动尖峰
constexpr double kMinZoomInertiaLogRate = 0.08;  // 低于此停止滑行

} // namespace

FreeGlobeController::FreeGlobeController(Camera* camera,
                                                 CameraConstraintSolver* solver)
    : camera_(camera), solver_(solver) {}

void FreeGlobeController::setViewport(int widthPixels, int heightPixels) {
    viewportWidth_ = std::max(1, widthPixels);
    viewportHeight_ = std::max(1, heightPixels);
}

void FreeGlobeController::setSurfacePicker(SurfacePicker picker) {
    surfacePicker_ = std::move(picker);
}

void FreeGlobeController::onDragStart(float xPixels, float yPixels,
                                          double timestamp) {
    // 抓取起始地表点；miss（按在地平线外/空白处）不再放弃整段拖拽，
    // 而是进入 spin 回退。grabSurfacePoint 内部已设置 hasGrabbedPoint_。
    grabSurfacePoint(xPixels, yPixels);
    dragging_ = true;
    dragStartX_ = xPixels;
    dragStartY_ = yPixels;
    dragLastX_ = xPixels;
    dragLastY_ = yPixels;
    inertiaAngularVelocity_ = 0.0;
    hasZoomInertia_ = false;   // 拖拽打断 zoom 惯性滑行
    zoomInertiaLogRate_ = 0.0;
    zoomSettleActive_ = false; // 拖拽打断滚轮平滑缩放
    dragVelocitySamples_.clear();
    pixelVelocitySamples_.clear();
    nearInertiaActive_ = false;
    lastDragTimestamp_ = timestamp;

    // 契约 1.2：起手判定模式一次，整段拖拽不切换；抓不到锚点（球外/天空）
    // 回退空间模式（spin 转台，与旧行为一致）。
    dragMode_ = hasGrabbedPoint_ ? resolveDragMode() : DragMode::Space;
    if (dragMode_ == DragMode::NearGround) {
        nearAnchorWorld_ = grabbedPoint_;
        nearAnchorNormal_ = grabbedNormal_;
        nearStartX_ = xPixels;
        nearStartY_ = yPixels;
        nearAppliedOffsetX_ = 0.0;
        nearAppliedOffsetY_ = 0.0;
        nearHorizonY_ = horizonScreenY();
        nearPixelsToHorizon_ = std::max(
            std::abs(static_cast<double>(yPixels) - nearHorizonY_), 1.0);
    }

    logCameraProbe("dragStart", xPixels, yPixels);
}

void FreeGlobeController::onDragMove(float xPixels, float yPixels,
                                         double timestamp) {
    if (!dragging_) return;

    if (dragMode_ == DragMode::NearGround) {
        applyNearGroundPan(xPixels, yPixels, timestamp);
    } else {
        applyAnchorDrag(xPixels, yPixels, timestamp);
    }

    dragLastX_ = xPixels;
    dragLastY_ = yPixels;

    logCameraProbe("dragMove", xPixels, yPixels);
}

void FreeGlobeController::onDragEnd() {
    if (!dragging_) return;
    // 清状态前吐 END:此刻 debugAnchorWorld 仍有效,手指终点用最后一次 move。
    logCameraProbe("dragEnd", dragLastX_, dragLastY_);
    dragging_ = false;
    hasGrabbedPoint_ = false;

    if (dragMode_ == DragMode::NearGround) {
        // 近地惯性（契约 1.4）：像素速度样本按 iOS 权重 0.6/0.35/0.05 合成，
        // 低于 100px/s 不触发（Mapbox/Flutter 双重证据）。
        constexpr double kWeights[3] = {0.6, 0.35, 0.05};
        const int n = static_cast<int>(pixelVelocitySamples_.size());
        double vx = 0.0;
        double vy = 0.0;
        double totalWeight = 0.0;
        for (int i = 0; i < n; ++i) {
            const PixelVelocitySample& s = pixelVelocitySamples_[i];
            const double w = kWeights[3 - n + i];
            if (s.dt > 0.0) {
                vx += static_cast<double>(s.dx) / s.dt * w;
                vy += static_cast<double>(s.dy) / s.dt * w;
            }
            totalWeight += w;
        }
        pixelVelocitySamples_.clear();
        if (n > 0 && totalWeight > 0.0) {
            vx /= totalWeight;
            vy /= totalWeight;
            if (std::hypot(vx, vy) >= kNearMinInertiaVelocityPxPerSec) {
                nearVelocityX_ = vx;
                nearVelocityY_ = vy;
                nearInertiaActive_ = true;
            }
        }
        return;
    }

    // iOS 风格松手速度（契约 1.4）：最近 ≤3 个相邻样本按 0.6/0.35/0.05
    // 加权（最旧样本权重最大，最新样本是"松开前一刻"的抖动值，权重最低；
    // 样本不足时对可用权重归一化）。方向锁定为加权速度方向，只衰减不反转。
    constexpr double kWeights[3] = {0.6, 0.35, 0.05};
    const int n = static_cast<int>(dragVelocitySamples_.size());
    glm::dvec3 velocity(0.0);
    double totalWeight = 0.0;
    for (int i = 0; i < n; ++i) {
        const DragVelocitySample& s = dragVelocitySamples_[i];
        const double w = kWeights[3 - n + i];  // 旧样本取靠前的高权重
        velocity += s.axis * (s.rate * w);
        totalWeight += w;
    }
    dragVelocitySamples_.clear();
    if (n == 0 || totalWeight <= 0.0) {
        inertiaAngularVelocity_ = 0.0;
        return;
    }
    velocity /= totalWeight;
    const double speed = glm::length(velocity);
    if (speed <= 1e-9) {
        inertiaAngularVelocity_ = 0.0;
        return;
    }
    inertiaAxis_ = glm::normalize(velocity);
    inertiaAngularVelocity_ =
        std::min(speed, kMaxInertiaAngularVelocityRadPerSec);
}

void FreeGlobeController::onDragCancel() {
    dragging_ = false;
    hasGrabbedPoint_ = false;
    dragVelocitySamples_.clear();
    clearGlideInertia();  // 立即停：pan + zoom 惯性全清（契约 1.5）
}

void FreeGlobeController::onPinchCancel() {
    pinching_ = false;
    hasPinchAnchor_ = false;
    pinchActiveMode_ = PinchMode::Undecided;
    clearGlideInertia();  // 立即停、不启动 zoom 惯性（契约 2.3）
}

FreeGlobeController::DragMode FreeGlobeController::resolveDragMode() const {
    const glm::dvec3 eye = camera_->position().raw();
    const double altitude =
        Ellipsoid::WGS84().cartesianToCartographic(Vec3(eye)).height();
    if (altitude >= kNearModeMaxAltitudeMeters) {
        return DragMode::Space;
    }
    const glm::dvec3 eyeNorm = glm::normalize(eye);
    const double cosPitch = std::clamp(
        glm::dot(-glm::normalize(camera_->direction().raw()), eyeNorm),
        -1.0, 1.0);
    const double pitch = std::acos(cosPitch);
    return pitch >= kNearModeMinPitchRadians ? DragMode::NearGround
                                             : DragMode::Space;
}

double FreeGlobeController::horizonScreenY() const {
    const glm::dvec3 eye = camera_->position().raw();
    const double eyeRadius = glm::length(eye);
    const double altitude = std::max(eyeRadius - kEarthRadiusMeters, 1.0);
    // 地平线俯角（相对当地水平）：δ = acos(R/(R+h))。
    const double delta = std::acos(
        std::clamp(kEarthRadiusMeters / (kEarthRadiusMeters + altitude),
                   -1.0, 1.0));
    const glm::dvec3 dir = glm::normalize(camera_->direction().raw());
    const glm::dvec3 eyeNorm = eye / eyeRadius;
    const double cosPitch = std::clamp(glm::dot(-dir, eyeNorm), -1.0, 1.0);
    const double pitch = std::acos(cosPitch);
    // 视线仰角 = 90°−pitch；地平线仰角 = −δ；屏幕 y 向下为正。
    const double offsetRad = (glm::half_pi<double>() - pitch) + delta;
    const double radPerPixel =
        camera_->verticalFovRadians() /
        static_cast<double>(std::max(1, viewportHeight_));
    if (radPerPixel <= 0.0) {
        return 1.0e9;
    }
    return static_cast<double>(viewportHeight_) * 0.5 +
           offsetRad / radPerPixel;
}

void FreeGlobeController::applyNearGroundPan(float xPixels, float yPixels,
                                             double timestamp) {
    // ① 地平线裁剪（契约 1.3）：请求偏移 = 手指相对起手的位移；总偏移
    // （含惯性）不得超过 0.75×地平线像素距离。按向量长度缩放、方向不变，
    // 绝不反向（MapLibre PR #6345 同款）。
    const double requestedX = static_cast<double>(xPixels) - nearStartX_;
    const double requestedY = static_cast<double>(yPixels) - nearStartY_;
    const double requestedLen = std::hypot(requestedX, requestedY);
    const double bound = kNearHorizonClampFactor * nearPixelsToHorizon_;
    double appliedX = requestedX;
    double appliedY = requestedY;
    if (bound > 0.0 && requestedLen > bound) {
        const double s = bound / requestedLen;
        appliedX = requestedX * s;
        appliedY = requestedY * s;
    }
    const double appliedDeltaX = appliedX - nearAppliedOffsetX_;
    const double appliedDeltaY = appliedY - nearAppliedOffsetY_;
    // 已停在边界（或没动）：不施加、不反向；方向折回时偏移减小、跟手恢复。
    if (std::abs(appliedDeltaX) < 1e-6 && std::abs(appliedDeltaY) < 1e-6) {
        lastDragTimestamp_ = timestamp;
        return;
    }

    // ② 有效手指位置 = 起手 + 已施加偏移；姿态锁定 ⇒ 射线→锚点切平面交点
    // 唯一。平移量 = 锚点 − 交点（平面平行位移 ⇒ 锚点保持在手指下）。
    const float fX = nearStartX_ + static_cast<float>(appliedX);
    const float fY = nearStartY_ + static_cast<float>(appliedY);
    const Ray ray = camera_->getPickRay(
        static_cast<double>(fX), static_cast<double>(fY),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    const glm::dvec3 o = ray.origin().raw();
    const glm::dvec3 d = ray.direction().raw();
    const glm::dvec3 n = nearAnchorNormal_.raw();
    const glm::dvec3 a = nearAnchorWorld_.raw();
    const double denom = glm::dot(d, n);
    if (std::abs(denom) < kNearPlaneGrazingEpsilon) {
        lastDragTimestamp_ = timestamp;
        return;  // 射线近平行/朝平面背面：不投影（地平线病态区）
    }
    const double t = glm::dot(a - o, n) / denom;
    if (t <= 0.0) {
        lastDragTimestamp_ = timestamp;
        return;  // 交点已在相机后方（越过地平线）
    }
    const glm::dvec3 p = o + d * t;
    const glm::dvec3 trans = a - p;
    camera_->setView(Vec3(camera_->position().raw() + trans),
                     camera_->direction(), camera_->up());
    clampNow(&a);

    nearAppliedOffsetX_ = appliedX;
    nearAppliedOffsetY_ = appliedY;

    // ③ 像素速度采样（最近 ≤3 个相邻样本，松手时 iOS 权重合成）。
    const double dt = timestamp - lastDragTimestamp_;
    if (dt > 0.0 && dt < 0.25) {
        PixelVelocitySample sample;
        sample.dt = dt;
        sample.dx = static_cast<float>(appliedDeltaX);
        sample.dy = static_cast<float>(appliedDeltaY);
        pixelVelocitySamples_.push_back(sample);
        if (pixelVelocitySamples_.size() > 3) {
            pixelVelocitySamples_.erase(pixelVelocitySamples_.begin());
        }
    }
    lastDragTimestamp_ = timestamp;
}

void FreeGlobeController::tickNearGroundInertia(double deltaSeconds) {
    const double speed = std::hypot(nearVelocityX_, nearVelocityY_);
    if (speed <= 1e-9) {
        nearInertiaActive_ = false;
        return;
    }
    // 停止判据（契约 1.4）：折合屏幕位移 < 0.5px/帧（与空间模式同规则）。
    if (speed * deltaSeconds < 0.5) {
        nearInertiaActive_ = false;
        nearVelocityX_ = 0.0;
        nearVelocityY_ = 0.0;
        return;
    }

    // 候选偏移（从起手累计）受地平线裁剪；方向锁定 ⇒ 只缩不放，绝不反向。
    double nextX = nearAppliedOffsetX_ + nearVelocityX_ * deltaSeconds;
    double nextY = nearAppliedOffsetY_ + nearVelocityY_ * deltaSeconds;
    const double nextLen = std::hypot(nextX, nextY);
    const double bound = kNearHorizonClampFactor * nearPixelsToHorizon_;
    if (bound > 0.0 && nextLen > bound) {
        const double s = bound / nextLen;
        nextX *= s;
        nextY *= s;
    }
    if (std::abs(nextX - nearAppliedOffsetX_) < 1e-6 &&
        std::abs(nextY - nearAppliedOffsetY_) < 1e-6) {
        nearInertiaActive_ = false;  // 已到地平线边界：停，不反向
        nearVelocityX_ = 0.0;
        nearVelocityY_ = 0.0;
        return;
    }

    const float fX = nearStartX_ + static_cast<float>(nextX);
    const float fY = nearStartY_ + static_cast<float>(nextY);
    const Ray ray = camera_->getPickRay(
        static_cast<double>(fX), static_cast<double>(fY),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    const glm::dvec3 o = ray.origin().raw();
    const glm::dvec3 d = ray.direction().raw();
    const glm::dvec3 n = nearAnchorNormal_.raw();
    const glm::dvec3 a = nearAnchorWorld_.raw();
    const double denom = glm::dot(d, n);
    if (std::abs(denom) < kNearPlaneGrazingEpsilon ||
        glm::dot(a - o, n) / denom <= 0.0) {
        nearInertiaActive_ = false;
        nearVelocityX_ = 0.0;
        nearVelocityY_ = 0.0;
        return;
    }
    const glm::dvec3 p = o + d * (glm::dot(a - o, n) / denom);
    const glm::dvec3 trans = a - p;
    camera_->setView(Vec3(camera_->position().raw() + trans),
                     camera_->direction(), camera_->up());
    clampNow(&a);
    nearAppliedOffsetX_ = nextX;
    nearAppliedOffsetY_ = nextY;

    // iOS 衰减：v *= 0.998^(dt_ms) ≈ exp(-2.0·dt)，方向锁定。
    const double decay = std::exp(-kInertiaDampingPerSecond * deltaSeconds);
    nearVelocityX_ *= decay;
    nearVelocityY_ *= decay;
}

void FreeGlobeController::onKeyCommand(
    InputEvent::Key key, const InputEvent::Modifiers& modifiers) {
    clearGlideInertia();  // 键盘是离散相机操作：先停惯性/滚轮平滑，避免混合自走
    const double kPanPixels = 100.0;  // Mapbox keyboard.js panStep
    switch (key) {
        case InputEvent::Key::ArrowLeft:
            if (modifiers.shift) {
                rotateHeadingByDegrees(-15.0);  // Mapbox bearingStep
            } else {
                panByPixels(-kPanPixels, 0.0);
            }
            break;
        case InputEvent::Key::ArrowRight:
            if (modifiers.shift) {
                rotateHeadingByDegrees(15.0);
            } else {
                panByPixels(kPanPixels, 0.0);
            }
            break;
        case InputEvent::Key::ArrowUp:
            if (modifiers.shift) {
                pitchByDegrees(10.0);  // Mapbox pitchStep
            } else {
                panByPixels(0.0, -kPanPixels);
            }
            break;
        case InputEvent::Key::ArrowDown:
            if (modifiers.shift) {
                pitchByDegrees(-10.0);
            } else {
                panByPixels(0.0, kPanPixels);
            }
            break;
        case InputEvent::Key::Plus:
            zoomByLevels(modifiers.shift ? 2.0 : 1.0);  // Mapbox: Shift 加倍
            break;
        case InputEvent::Key::Minus:
            zoomByLevels(modifiers.shift ? -2.0 : -1.0);
            break;
        default:
            break;
    }
}

void FreeGlobeController::panByPixels(double dx, double dy) {
    const float cx = static_cast<float>(viewportWidth_) * 0.5f;
    const float cy = static_cast<float>(viewportHeight_) * 0.5f;
    if (resolveDragMode() == DragMode::NearGround) {
        // 近地：切平面平移（与单指拖拽同一条路径，含地平线裁剪）。
        // 近地高倾斜时屏幕中心射线常指向天空，先取中心、再取下半屏可见地表。
        const float grabY = static_cast<float>(viewportHeight_) * 0.9f;
        float anchorY = cy;
        bool got = grabSurfacePoint(cx, cy);
        if (!got) {
            got = grabSurfacePoint(cx, grabY);
            anchorY = grabY;
        }
        if (!got || !hasGrabbedPoint_) {
            // 中心与下半屏都取不到地表（异常姿态）：回退空间转台。
            const glm::dquat delta = turntableDeltaFromPixels(dx, dy);
            camera_ops::rotateAboutOrigin(*camera_, delta);
            clampNow(nullptr);
            return;
        }
        dragMode_ = DragMode::NearGround;
        nearAnchorWorld_ = grabbedPoint_;
        nearAnchorNormal_ = grabbedNormal_;
        nearStartX_ = cx;
        nearStartY_ = anchorY;
        nearAppliedOffsetX_ = 0.0;
        nearAppliedOffsetY_ = 0.0;
        nearHorizonY_ = horizonScreenY();
        nearPixelsToHorizon_ = std::max(
            std::abs(static_cast<double>(anchorY) - nearHorizonY_), 1.0);
        applyNearGroundPan(cx + static_cast<float>(dx),
                           anchorY + static_cast<float>(dy), 0.0);
        hasGrabbedPoint_ = false;  // 键盘平移是瞬时的，不留拖拽状态
    } else {
        // 空间：转台旋转（拖球语义的离散一步）。
        const glm::dquat delta = turntableDeltaFromPixels(dx, dy);
        camera_ops::rotateAboutOrigin(*camera_, delta);
        clampNow(nullptr);
    }
}

void FreeGlobeController::zoomByLevels(double levels) {
    const float cx = static_cast<float>(viewportWidth_) * 0.5f;
    const float cy = static_cast<float>(viewportHeight_) * 0.5f;
    const double stepScale = std::exp(levels * std::log(2.0));
    if (grabSurfacePoint(cx, cy) && hasGrabbedPoint_) {
        const glm::dvec3 anchor = grabbedPoint_.raw();
        const glm::dvec3 eye = camera_->position().raw();
        const glm::dvec3 toAnchor = anchor - eye;
        const double dist = glm::length(toAnchor);
        if (dist > 1e-6) {
            const double moveMeters = dist * (1.0 - 1.0 / stepScale);
            glm::dvec3 nextEye = eye + (toAnchor / dist) * moveMeters;
            if ((glm::length(nextEye) / kEarthRadiusMeters) <=
                kMaxDistanceEarthRadii) {
                camera_->setView(Vec3(nextEye), camera_->direction(),
                                 camera_->up());
            }
            clampNow(&anchor);
        }
        hasGrabbedPoint_ = false;
    } else {
        const double moveMeters =
            camera_->position().length() * (1.0 - 1.0 / stepScale);
        const glm::dvec3 nextEye =
            camera_->position().raw() +
            camera_->direction().raw() * moveMeters;
        if ((glm::length(nextEye) / kEarthRadiusMeters) <=
            kMaxDistanceEarthRadii) {
            camera_->setView(Vec3(nextEye), camera_->direction(),
                             camera_->up());
        }
        clampNow(nullptr);
    }
}

void FreeGlobeController::rotateHeadingByDegrees(double degrees) {
    camera_ops::rotateAboutPoint(*camera_, camera_->position().raw(),
                                 camera_->up().raw(),
                                 glm::radians(degrees));
    clampNow(nullptr);
}

void FreeGlobeController::pitchByDegrees(double degrees) {
    const float cx = static_cast<float>(viewportWidth_) * 0.5f;
    const float cy = static_cast<float>(viewportHeight_) * 0.5f;
    const double rad = glm::radians(degrees);
    const glm::dvec3 eye = camera_->position().raw();
    if (grabSurfacePoint(cx, cy) && hasGrabbedPoint_) {
        rotateCameraVerticalAroundPoint(grabbedPoint_.raw(), rad,
                                        kTouchMinSlope);
    } else {
        rotateCameraVerticalAroundPoint(eye, rad, kTouchMinSlope);
    }
    hasGrabbedPoint_ = false;
}

void FreeGlobeController::onPinchGesture(const PinchInput& input) {
    // Pinch starts/updates interrupt drag inertia; mixed inertias feel unstable.
    inertiaAngularVelocity_ = 0.0;
    dragging_ = false;
    hasGrabbedPoint_ = false;

    const bool isPinchStartFrame = !pinching_;

    if (isPinchStartFrame) {
        pinching_ = true;
        // 新捏合打断上一段 zoom 惯性滑行，并重置速率累积。
        hasZoomInertia_ = false;
        zoomInertiaLogRate_ = 0.0;
        if (!input.smoothZoom) {
            zoomSettleActive_ = false;  // 双指捏合打断滚轮平滑缩放
        }
        lastPinchTimestamp_ = input.timestamp;
        pinchAppliedScaleLog_ = 0.0;
        pinchAppliedTwistRadians_ = 0.0;
        pinchActiveMode_ = PinchMode::Undecided;
        pitchAppliedRadians_ = 0.0;
        pitchBaselineY_ = input.centroidY;
        pinchAnchorScreenX_ = input.centroidX;
        pinchAnchorScreenY_ = input.centroidY;
        lastPinchCentroidX_ = input.centroidX;
        lastPinchCentroidY_ = input.centroidY;

        hasPinchAnchor_ =
            tryAcquirePinchAnchor(input.centroidX, input.centroidY);
        platformLog(LogLevel::Info, "CameraCtrl",
            "pinchStart hasAnchor=%d center=(%.0f,%.0f)",
            hasPinchAnchor_, input.centroidX, input.centroidY);
        logCameraProbe("pinchStart", input.centroidX, input.centroidY);
    }

    inertiaAngularVelocity_ = 0.0;

    // mode 迁移（契约 2.2 组合手势）：PinchMode 只是"倾斜轴是否启用"的锁，
    // 缩放/旋转/平移由 InputManager 的每轴激活标志独立门控。Undecided→
    // Manipulate/Pitch 无需特殊处理——pin 是状态求解而非增量累加，pin 目标
    // 切到当前质心后，窗口期积压的质心行程被一次精确补齐（上界=pan 阈值
    // 8dp，肉眼不可见），不丢量、不需要回滚。Pitch 起手锁只重取倾斜基线。
    if (input.mode != pinchActiveMode_) {
        if (input.mode == PinchMode::Pitch) {
            pitchBaselineY_ = input.centroidY;
            pitchAppliedRadians_ = 0.0;
        }
        pinchActiveMode_ = input.mode;
    }

    // 缩放轴（契约 2.2）：未达阈值（0.1 log2）前不缩放，达到后整段激活。
    // Jerk 限幅（绝对量状态表述）：本事件增量 = 请求累计 − 已施加累计，
    // 夹到 ±ln(1.3)；已施加值对请求值的落后量夹到 ±kMaxPinchScaleResidualLog。
    double stepScale = 1.0;
    if (input.zoomEngaged && !input.smoothZoom) {
        const double jerkStepLimit = std::log(1.0 + kTouchJerkLimit);
        const double requestedLog =
            std::log(std::max(static_cast<double>(input.scaleFromStart), 1e-6));
        double newAppliedLog = pinchAppliedScaleLog_ +
            std::clamp(requestedLog - pinchAppliedScaleLog_,
                       std::log(1.0 - kTouchJerkLimit), jerkStepLimit);
        newAppliedLog = std::clamp(newAppliedLog,
                                   requestedLog - kMaxPinchScaleResidualLog,
                                   requestedLog + kMaxPinchScaleResidualLog);
        stepScale = std::exp(newAppliedLog - pinchAppliedScaleLog_);
        pinchAppliedScaleLog_ = newAppliedLog;
    }

    // 无锚起手（球外/pick 全 miss）后每事件重试获取，成功即转入有锚分支。
    if (!hasPinchAnchor_) {
        hasPinchAnchor_ =
            tryAcquirePinchAnchor(input.centroidX, input.centroidY);
    }

    // 滚轮平滑缩放（契约 3.1）：把本格对数增量并入目标，交由 tick 指数收敛
    // （~300ms、单帧上限 ±ln(2)、不越目标不反向）；本事件不瞬时施加。
    // 无锚（天空/太空像素）时锚点取 eye 前方 |eye| 处——沿视线平滑缩放。
    if (input.smoothZoom) {
        const double notchLog = std::log(std::max(
            static_cast<double>(input.scaleFromStart), 1e-6));
        if (!zoomSettleActive_) {
            zoomSettleActive_ = true;
            zoomSettleAppliedLog_ = 0.0;
            zoomSettleTargetLog_ = 0.0;
        }
        if (hasPinchAnchor_) {
            zoomSettleAnchor_ =
                pinchAnchorNormal_.raw() * grabbedRadiusMeters_;
        } else {
            const glm::dvec3 eye = camera_->position().raw();
            zoomSettleAnchor_ =
                eye + camera_->direction().raw() * glm::length(eye);
        }
        zoomSettleTargetLog_ += notchLog;
        stepScale = 1.0;
    }

    if (hasPinchAnchor_) {
        const glm::dvec3 anchorWorld =
            pinchAnchorNormal_.raw() * grabbedRadiusMeters_;

        // ① dolly：沿 eye→anchor 直线把距离缩到 d/s（位移 d*(1-1/s)）。
        // 精确保锚：eye→anchor 方向不变、dir/up 不变 ⇒ 锚点像素不动。
        const glm::dvec3 eye = camera_->position().raw();
        const glm::dvec3 toAnchor = anchorWorld - eye;
        const double distanceToAnchor = glm::length(toAnchor);
        const double moveMeters =
            distanceToAnchor * (1.0 - 1.0 / stepScale);
        glm::dvec3 nextEye = distanceToAnchor > 1e-6
            ? eye + (toAnchor / distanceToAnchor) * moveMeters
            : eye;
        if ((glm::length(nextEye) / kEarthRadiusMeters) <=
            kMaxDistanceEarthRadii) {
            camera_->setView(Vec3(nextEye), camera_->direction(),
                             camera_->up());
        }
        clampNow(&anchorWorld);

        // 累积 zoom 惯性速率（对数距离空间，EMA 平滑）。stepScale≈1 的纯
        // 旋转/平移帧会让速率自然衰减向 0，只有真正在缩放才留下滑行动量。
        {
            const double dt = input.timestamp - lastPinchTimestamp_;
            if (dt > 0.0 && dt < 0.25) {
                const double instRate = std::clamp(
                    std::log(stepScale) / dt,
                    -kMaxZoomInertiaLogRate, kMaxZoomInertiaLogRate);
                zoomInertiaLogRate_ =
                    zoomInertiaLogRate_ * (1.0 - kVelocitySmoothing) +
                    instRate * kVelocitySmoothing;
            }
            zoomInertiaAnchor_ = anchorWorld;
        }

        // ② twist（契约 2.2）：弧长 ≥25px 激活后绕锚点法线旋转；锚点在轴上
        // ⇒ 精确保锚。阈值防近距手指误转，激活后每帧增量直接施加。
        if (input.rotateEngaged) {
            const double twistStep = static_cast<double>(
                input.twistFromStartRadians) - pinchAppliedTwistRadians_;
            pinchAppliedTwistRadians_ =
                static_cast<double>(input.twistFromStartRadians);
            if (std::abs(twistStep) > 0.0) {
                camera_ops::rotateAboutPoint(*camera_, anchorWorld,
                                             pinchAnchorNormal_.raw(),
                                             twistStep);
            }
        }

        // ③ pitch（仅 Pitch 模式）：质心 Y 相对基线的绝对映射，绕锚点
        // 转相机竖直面（精确保锚）。反 wind-up：只把实际施加的量计入
        // pitchAppliedRadians_，被守卫拒绝时重取基线——到达俯仰界后反向
        // 立即响应，零死区离合。
        if (pinchActiveMode_ == PinchMode::Pitch) {
            const double desired = -kPinchTiltRadiansPerPixel *
                static_cast<double>(input.centroidY - pitchBaselineY_);
            const double tiltStep = std::clamp(
                desired - pitchAppliedRadians_,
                -kPinchTiltMaxStepRadians, kPinchTiltMaxStepRadians);
            if (std::abs(tiltStep) > 1e-12) {
                if (rotateCameraVerticalAroundPoint(anchorWorld, tiltStep,
                                                    kTouchMinSlope)) {
                    pitchAppliedRadians_ += tiltStep;
                } else {
                    // 重基线：让 desired(当前质心) == 已施加量。
                    pitchBaselineY_ = input.centroidY +
                        static_cast<float>(pitchAppliedRadians_ /
                                           kPinchTiltRadiansPerPixel);
                }
            }
        }

        // ④ pin：唯一产生横向世界运动的通道（契约 2.2 组合——Pitch 锁只
        // 启用倾斜轴，平移与缩放/旋转一样随动）。Undecided 钉起手质心，
        // 一旦任一轴激活钉当前质心（刚性 pan 内建）。
        float pinTargetX = input.centroidX;
        float pinTargetY = input.centroidY;
        if (pinchActiveMode_ == PinchMode::Undecided) {
            pinTargetX = pinchAnchorScreenX_;
            pinTargetY = pinchAnchorScreenY_;
        }
        applyPinchPin(pinTargetX, pinTargetY);
    } else {
        // 无有效 pinch anchor 时，沿视线方向缩放相机（无锚点可钉，只能以
        // 地心距当作被缩放的距离）。位移量与有锚点分支同一公式 d*(1-1/s)，
        // 否则缩进过冲、拉远不足。
        const double moveMeters =
            camera_->position().length() * (1.0 - 1.0 / stepScale);
        glm::dvec3 nextEye =
            camera_->position().raw() +
            camera_->direction().raw() * moveMeters;
        if ((glm::length(nextEye) / kEarthRadiusMeters) <= kMaxDistanceEarthRadii) {
            camera_->setView(Vec3(nextEye), camera_->direction(), camera_->up());
        }
        clampNow(nullptr);
    }

    lastPinchCentroidX_ = input.centroidX;
    lastPinchCentroidY_ = input.centroidY;
    lastPinchTimestamp_ = input.timestamp;

    logCameraProbe("pinchMove", input.centroidX, input.centroidY);
}

void FreeGlobeController::onPinchEnd() {
    // 清状态前吐 END:pinching_/hasPinchAnchor_ 尚为真,锚点仍可读。
    logCameraProbe("pinchEnd", lastPinchCentroidX_, lastPinchCentroidY_);
    pinching_ = false;
    // 契约 2.3：双指 pan 无惯性（直接操纵，松手即停）；仅当缩放留有足够
    // 动量时启动 zoom 惯性滑行（锚点仍需保留以沿视线朝它 dolly）。
    if (hasPinchAnchor_ &&
        std::abs(zoomInertiaLogRate_) >= kMinZoomInertiaLogRate) {
        hasZoomInertia_ = true;
    } else {
        hasZoomInertia_ = false;
        zoomInertiaLogRate_ = 0.0;
    }
    hasPinchAnchor_ = false;
}

void FreeGlobeController::onActivate() {
    // 本控制器无需从位姿反解任何东西(位姿是唯一真值),对齐 = 清空手势期瞬时量。
    dragging_ = false;
    hasGrabbedPoint_ = false;
    pinching_ = false;
    hasPinchAnchor_ = false;
    dragMode_ = DragMode::Space;
    nearInertiaActive_ = false;
    clearAllInertia();
}

void FreeGlobeController::onDeactivate() {
    onActivate();  // 同一套清理:瞬时量不跨控制器存活
}

void FreeGlobeController::tick(double deltaSeconds) {
    // Flick inertia is velocity-based only: the released angular velocity
    // (rad/s, dt-scaled and exponentially damped below) continues the pan.
    // The previous quaternion "touch inertia" re-applied ~s^3 of the LAST
    // drag event's full rotation EVERY FRAME (~36x the event delta in total,
    // frame-rate dependent), which flung the camera hundreds of kilometers
    // after one swipe when input events were coalesced under load.
    if (!dragging_ &&
        inertiaAngularVelocity_ > kMinInertiaAngularVelocity &&
        deltaSeconds > 0.0) {
        double angle = inertiaAngularVelocity_ * deltaSeconds;
        // 停止判据（Cesium 规则，契约 1.4）：折合屏幕位移 < 0.5px/帧即停。
        // 与 isAnimating 共用 kMinInertiaAngularVelocity 作为归零 epsilon，
        // 但真实停止量是像素——视口越大可接受的角度越小。
        const double radPerPixel =
            camera_->verticalFovRadians() /
            static_cast<double>(std::max(1, viewportHeight_));
        if (radPerPixel > 0.0 && angle / radPerPixel < 0.5) {
            inertiaAngularVelocity_ = 0.0;
        } else {
            glm::dquat delta = glm::angleAxis(angle, inertiaAxis_);
            camera_ops::rotateAboutOrigin(*camera_, delta);
            clampNow(nullptr);
            inertiaAngularVelocity_ *=
                std::exp(-kInertiaDampingPerSecond * deltaSeconds);
            if (inertiaAngularVelocity_ <= kMinInertiaAngularVelocity) {
                inertiaAngularVelocity_ = 0.0;
            }
        }
    }

    // 近地拖图惯性（契约 1.4）：像素速度沿锁定方向滑行，受地平线裁剪。
    if (!dragging_ && !pinching_ && nearInertiaActive_ && deltaSeconds > 0.0) {
        tickNearGroundInertia(deltaSeconds);
    }

    // 滚轮平滑缩放（契约 3.1）：指数收敛到目标，单帧上限 ±ln(2)。
    if (!dragging_ && !pinching_ && zoomSettleActive_ && deltaSeconds > 0.0) {
        const double remaining =
            zoomSettleTargetLog_ - zoomSettleAppliedLog_;
        if (std::abs(remaining) < 1e-6) {
            zoomSettleActive_ = false;
        } else {
            const double ln2 = std::log(2.0);
            const double cap = kMaxZoomLevelsPerFrame * ln2;
            double step = remaining *
                (1.0 - std::exp(-kZoomSettleRatePerSecond * deltaSeconds));
            step = std::clamp(step, -cap, cap);
            if (std::abs(step) < 1e-9) {
                step = remaining;  // 收敛到目标（指数永不到达，最后一步直接结算）
            }
            const double stepScale = std::exp(step);
            const glm::dvec3 eye = camera_->position().raw();
            const glm::dvec3 toAnchor = zoomSettleAnchor_ - eye;
            const double dist = glm::length(toAnchor);
            if (dist > 1e-3) {
                const double moveMeters = dist * (1.0 - 1.0 / stepScale);
                glm::dvec3 nextEye = eye + (toAnchor / dist) * moveMeters;
                if ((glm::length(nextEye) / kEarthRadiusMeters) <=
                    kMaxDistanceEarthRadii) {
                    camera_->setView(Vec3(nextEye), camera_->direction(),
                                     camera_->up());
                }
                clampNow(&zoomSettleAnchor_);
            }
            zoomSettleAppliedLog_ += step;
            if (std::abs(zoomSettleTargetLog_ - zoomSettleAppliedLog_) <
                1e-6) {
                zoomSettleActive_ = false;
            }
        }
    }

    // Zoom 惯性滑行：沿视线朝锚点按对数距离指数逼近（distance*=exp(-r·dt)），
    // 永不越过锚点、天然有界；沿 eye→anchor 直线 dolly 故锚点保持钉住。
    if (!dragging_ && !pinching_ && hasZoomInertia_ && deltaSeconds > 0.0) {
        const glm::dvec3 eye = camera_->position().raw();
        const glm::dvec3 toEye = eye - zoomInertiaAnchor_;
        const double dist = glm::length(toEye);
        if (dist > 1e-3) {
            const double sFrame =
                std::exp(zoomInertiaLogRate_ * deltaSeconds);  // >1 拉近
            glm::dvec3 nextEye = zoomInertiaAnchor_ + toEye / sFrame;
            if ((glm::length(nextEye) / kEarthRadiusMeters) <=
                kMaxDistanceEarthRadii) {
                camera_->setView(Vec3(nextEye), camera_->direction(),
                                 camera_->up());
                }
            clampNow(&zoomInertiaAnchor_);
        }
        zoomInertiaLogRate_ *=
            std::exp(-kZoomInertiaDampingPerSecond * deltaSeconds);
        if (std::abs(zoomInertiaLogRate_) < kMinZoomInertiaLogRate) {
            hasZoomInertia_ = false;
            zoomInertiaLogRate_ = 0.0;
        }
    }

}

void FreeGlobeController::clearPanInertia() {
    inertiaAngularVelocity_ = 0.0;
    dragVelocitySamples_.clear();
    nearInertiaActive_ = false;
    nearVelocityX_ = 0.0;
    nearVelocityY_ = 0.0;
    pixelVelocitySamples_.clear();
}

void FreeGlobeController::clearGlideInertia() {
    clearPanInertia();
    hasZoomInertia_ = false;
    zoomInertiaLogRate_ = 0.0;
    zoomSettleActive_ = false;
    zoomSettleTargetLog_ = 0.0;
    zoomSettleAppliedLog_ = 0.0;
}

void FreeGlobeController::clearAllInertia() {
    clearGlideInertia();
}

bool FreeGlobeController::clampNow(const glm::dvec3* pinnedAnchorWorld) {
    // 手势/惯性路径：调用方刚刚显式动过相机，所以恒是 user-driven（突变滤波
    // 立即生效），也没有帧间隔可言（dt=0，数据驱动的指数衰减不参与）。
    // 与帧末哨兵是两条性质不同的路径，别再合并回一个带 source 枚举的函数。
    const glm::dvec3 eye = camera_->position().raw();
    const glm::dvec3 clamped = solver_->constrainEye(
        eye, /*userDriven=*/true, /*deltaSeconds=*/0.0, pinnedAnchorWorld);
    const bool changed = glm::length(clamped - eye) > 1e-6;
    if (changed) {
        camera_->setView(Vec3(clamped), camera_->direction(), camera_->up());
    }
    solver_->commitPose(camera_->position().raw(), camera_->direction().raw());
    return changed;
}

bool FreeGlobeController::rotateCameraVerticalAroundPoint(
    const glm::dvec3& center, double angle, double minSlope) {
    const glm::dvec3 axis = camera_->right().raw();
    if (glm::length(axis) < 1e-10 || std::abs(angle) < 1e-12) {
        return false;
    }

    const glm::dvec3 currentEyeNorm = glm::normalize(camera_->position().raw());
    const double currentSlope = glm::dot(-camera_->direction().raw(), currentEyeNorm);

    const glm::dquat delta = glm::angleAxis(angle, glm::normalize(axis));
    const glm::dvec3 nextEye = center + delta * (camera_->position().raw() - center);
    const glm::dvec3 nextDirection = delta * camera_->direction().raw();
    const glm::dvec3 nextUp = delta * camera_->up().raw();

    const glm::dvec3 nextEyeNorm = glm::normalize(nextEye);
    const double nextSlope = glm::dot(-nextDirection, nextEyeNorm);
    if (glm::dot(nextUp, nextEyeNorm) <= 0.0) {
        return false;
    }

    // 地形净空守卫(与 minSlope 守卫同构:只拒绝"让净空更差"的方向,反向立即
    // 响应——调用方按被拒处理重基线,零死区离合)。用滤波地形高做廉价预判,
    // 不重采样。拒绝而非事后顶起:顶起要么破坏 Pitch 的锚点像素不变量,
    // 要么(Cesium 式旋转补偿)偷偷改 direction,都不如"停住"。
    {
        const auto& ell = Ellipsoid::WGS84();
        const double hNext =
            ell.cartesianToCartographic(Vec3(nextEye)).height();
        if (hNext < kMaxTerrainHeightMeters + kMinAltitudeMeters) {
            const double minHeight =
                std::max(solver_->filteredTerrainHeight(), 0.0) +
                kMinAltitudeMeters;
            const double hCur =
                ell.cartesianToCartographic(camera_->position()).height();
            if (hNext < minHeight && hNext < hCur) {
                return false;
            }
        }
    }

    if (minSlope > 0.0) {
        const double dSlope = nextSlope - currentSlope;
        if (nextSlope < minSlope && dSlope < 0.0) {
            return false;
        }

        const bool canApply =
            (nextSlope > 0.1 && glm::dot(nextUp, nextEyeNorm) > 0.0) ||
            currentSlope <= 0.1 ||
            glm::dot(camera_->up().raw(), currentEyeNorm) <= 0.0;
        if (!canApply) {
            return false;
        }
    }

    camera_->setView(Vec3(nextEye), Vec3(nextDirection), Vec3(nextUp));
    return true;
}

void FreeGlobeController::logCameraProbe(const char* phase,
                                         double fingerX,
                                         double fingerY) const {
    if (camera_ == nullptr) return;
    const Mat4 vp = camera_->viewProjectionMatrix(
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    const double* m = vp.data();  // 16 doubles，列主序
    Vec3 anchor(0.0, 0.0, 0.0);
    const bool hasAnchor = debugAnchorWorld(anchor);
    // eyeAlt:相机椭球高。近碰撞压测用——证明 clampNow 真顶住地面(高度贴
    // kMinAltitudeMeters)时 anchorErr 是否仍守住(现版保锚 clamp 应守住)。
    const double eyeAlt =
        Ellipsoid::WGS84().cartesianToCartographic(camera_->position()).height();
    platformLog(LogLevel::Info, "CAMPROBE",
        "%s finger=(%.1f,%.1f) vp=%dx%d eyeAlt=%.2f anchor=%d,%.9g,%.9g,%.9g "
        "vpm=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
        phase, fingerX, fingerY, viewportWidth_, viewportHeight_, eyeAlt,
        hasAnchor ? 1 : 0, anchor.x(), anchor.y(), anchor.z(),
        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
        m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

bool FreeGlobeController::debugAnchorWorld(Vec3& outWorld) const {
    if (pinching_ && hasPinchAnchor_) {
        outWorld = Vec3(pinchAnchorNormal_.raw() * grabbedRadiusMeters_);
        return true;
    }
    if (dragging_ && hasGrabbedPoint_) {
        outWorld = grabbedPoint_;
        return true;
    }
    return false;
}

FreeGlobeController::AnchorSolveResult
FreeGlobeController::solveAnchorRotation(const Vec3& anchorNormal,
                                             float xPixels,
                                             float yPixels) const {
    AnchorSolveResult result;

    const Ray ray = camera_->getPickRay(
        static_cast<double>(xPixels),
        static_cast<double>(yPixels),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    Vec3 pointOnSphere;
    if (!pointOnGrabSphere(ray, pointOnSphere, result.hit)) {
        return result;
    }
    result.valid = true;

    const glm::dvec3 from = pointOnSphere.normalized().raw();
    const glm::dvec3 to = anchorNormal.raw();
    result.conditioning = std::abs(glm::dot(ray.direction().raw(), from));

    glm::dvec3 axis = glm::cross(from, to);
    const double axisLength = glm::length(axis);
    if (axisLength < 1e-10) {
        result.degenerate = true;
        return result;
    }

    const double dot = std::clamp(glm::dot(from, to), -1.0, 1.0);
    const double angle = std::atan2(axisLength, dot);
    result.delta = glm::angleAxis(angle, axis / axisLength);
    return result;
}

bool FreeGlobeController::intersectGrabSphere(const Ray& ray,
                                                  Vec3& outPoint) const {
    return intersectSphere(ray, grabbedRadiusMeters_, outPoint);
}

bool FreeGlobeController::pointOnGrabSphere(const Ray& ray,
                                                Vec3& outPoint,
                                                bool& outTrueHit) const {
    if (intersectGrabSphere(ray, outPoint)) {
        outTrueHit = true;
        return true;
    }
    outTrueHit = false;
    // 最近接近点：球面上离射线最近的点。t* 处射线与"球心→该点"方向正交，
    // 相切时与真交点重合 → 解在跨球缘时 C0 连续。
    const glm::dvec3 o = ray.origin().raw();
    const glm::dvec3 d = ray.direction().raw();
    const double tStar = -glm::dot(o, d);
    if (tStar <= 0.0) {
        return false;  // 球心在射线后方（背对地球看天）
    }
    const glm::dvec3 q = o + d * tStar;
    const double qLen = glm::length(q);
    if (qLen < 1e-6) {
        return false;  // 射线（数值上）穿过地心，方向无定义
    }
    outPoint = Vec3(q * (grabbedRadiusMeters_ / qLen));
    return true;
}

glm::dquat FreeGlobeController::turntableDeltaFromPixels(double dx,
                                                             double dy) const {
    // 屏幕中心处每像素约对应的角度，给出接近 1:1 的转台手感。
    // 水平/垂直每像素角度相同（aspect 抵消），故统一用 fov/height。
    const double radPerPixel =
        camera_->verticalFovRadians() /
        static_cast<double>(std::max(1, viewportHeight_));
    // 手指右移 → 世界右转（绕屏幕竖轴=camera up）；
    // 手指下移 → 世界下转（绕屏幕横轴=camera right）。
    const glm::dquat yaw =
        glm::angleAxis(-dx * radPerPixel, camera_->up().raw());
    const glm::dquat pitch =
        glm::angleAxis(-dy * radPerPixel, camera_->right().raw());
    return glm::normalize(yaw * pitch);
}

glm::dquat FreeGlobeController::spinTurntableDelta(float xPixels,
                                                       float yPixels) const {
    return turntableDeltaFromPixels(
        static_cast<double>(xPixels) - dragLastX_,
        static_cast<double>(yPixels) - dragLastY_);
}

bool FreeGlobeController::tryAcquirePinchAnchor(float xPixels,
                                                    float yPixels) {
    grabbedRadiusMeters_ = kEarthRadiusMeters;
    Vec3 picked;
    if (!pickSurfacePoint(xPixels, yPixels, picked)) {
        return false;
    }
    // 半径钳到 eye 以下：锚点高于相机（峰顶锚点 vs 谷地相机）时抓取球会
    // 包住 eye，此后每条射线都"命中"球背面，pin 给出近 π 的疯狂旋转。
    const double eyeRadius = camera_->position().length();
    grabbedRadiusMeters_ = std::min(
        picked.length(),
        std::max(eyeRadius - kGrabSphereEyeMarginMeters, 1.0));
    // 锚点必须在拾取射线上：PickingService 的地形拾取返回点不在射线上，
    // 落差会被首个 pin 一次性补掉＝起手跳变（真机实测 227~471px）。方向
    // 换成射线∩钳位球的交点（miss 取最近接近点），半径保留地形高（除非被
    // 上面的 eye 钳位收紧）。
    const Ray ray = camera_->getPickRay(
        static_cast<double>(xPixels),
        static_cast<double>(yPixels),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    Vec3 onSphere;
    bool trueHit = false;
    if (!pointOnGrabSphere(ray, onSphere, trueHit)) {
        return false;
    }
    pinchAnchorNormal_ = onSphere.normalized();
    return true;
}

glm::dquat FreeGlobeController::applyPinchPin(float targetX,
                                                  float targetY) {
    const glm::dquat kIdentity{1.0, 0.0, 0.0, 0.0};
    const AnchorSolveResult solve =
        solveAnchorRotation(pinchAnchorNormal_, targetX, targetY);
    bool regrab = false;
    glm::dquat delta = kIdentity;
    if (solve.valid) {
        const double w = anchorExactWeight(solve.conditioning);
        if (w >= 1.0) {
            if (solve.degenerate) {
                return kIdentity;  // 锚点已在目标像素下（纯缩放/拧动帧）
            }
            delta = solve.delta;
        } else {
            // 病态区（质心到球缘/球外）：与单指同一套连续化——混入质心
            // 转台，应用后整点重取锚点。
            delta = glm::slerp(
                turntableDeltaFromPixels(
                    static_cast<double>(targetX) - lastPinchCentroidX_,
                    static_cast<double>(targetY) - lastPinchCentroidY_),
                solve.delta, w);
            regrab = true;
        }
    } else {
        // 极端退化（球心在射线后方）：纯质心转台。
        delta = turntableDeltaFromPixels(
            static_cast<double>(targetX) - lastPinchCentroidX_,
            static_cast<double>(targetY) - lastPinchCentroidY_);
    }
    camera_ops::rotateAboutOrigin(*camera_, delta);

    // pin 是唯一的横向运动通道，山区双指横移可能把 eye 转进地形——钉合后
    // 必须做碰撞解算（dolly 分支已各自解过）。
    {
        const glm::dvec3 anchorWorld =
            pinchAnchorNormal_.raw() * grabbedRadiusMeters_;
        clampNow(&anchorWorld);
    }

    if (regrab) {
        const Ray ray = camera_->getPickRay(
            static_cast<double>(targetX),
            static_cast<double>(targetY),
            static_cast<double>(viewportWidth_),
            static_cast<double>(viewportHeight_));
        Vec3 regrabbed;
        bool trueHit = false;
        if (pointOnGrabSphere(ray, regrabbed, trueHit)) {
            pinchAnchorNormal_ = regrabbed.normalized();
        }
    }
    return delta;
}

bool FreeGlobeController::intersectSphere(const Ray& ray,
                                              double radiusMeters,
                                              Vec3& outPoint) {
    const glm::dvec3 o = ray.origin().raw();
    const glm::dvec3 d = ray.direction().raw();
    const double b = 2.0 * glm::dot(o, d);
    const double c = glm::dot(o, o) - radiusMeters * radiusMeters;
    const double disc = b * b - 4.0 * c;
    if (disc < 0.0) {
        return false;
    }

    const double sqrtDisc = std::sqrt(disc);
    const double t0 = (-b - sqrtDisc) * 0.5;
    const double t1 = (-b + sqrtDisc) * 0.5;
    const double t = t0 > 0.0 ? t0 : t1;
    if (t <= 0.0) {
        return false;
    }

    outPoint = ray.pointAt(t);
    return true;
}

bool FreeGlobeController::pickSurfacePoint(float xPixels, float yPixels,
                                               Vec3& outPoint) const {
    if (surfacePicker_ && surfacePicker_(xPixels, yPixels, outPoint)) {
        return true;
    }

    const Ray ray = camera_->getPickRay(
        static_cast<double>(xPixels),
        static_cast<double>(yPixels),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    return intersectGrabSphere(ray, outPoint);
}

bool FreeGlobeController::grabSurfacePoint(float xPixels, float yPixels) {
    grabbedRadiusMeters_ = kEarthRadiusMeters;

    const Ray ray = camera_->getPickRay(
        static_cast<double>(xPixels),
        static_cast<double>(yPixels),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));

    Vec3 grabbedPoint;
    if (pickSurfacePoint(xPixels, yPixels, grabbedPoint)) {
        // 半径取拾取点（保留地形高），但钳到 eye 半径以下——锚点高于相机时
        // 抓取球会包住 eye，射线全部命中球背面，锚定数学瞬间疯转。
        const double eyeRadius = camera_->position().length();
        grabbedRadiusMeters_ = std::min(
            grabbedPoint.length(),
            std::max(eyeRadius - kGrabSphereEyeMarginMeters, 1.0));
    }
    // 否则起手在球外：半径保持标准球，取最近接近点当锚。整段拖拽由条件数
    // 混合连续处理——球外 w≈0 纯转台（旧 spin 手感不变），扫回球面时 w 连续
    // 升回 1、锚定平滑恢复（旧实现 miss 即永久 latch 到抬手 = 问题3）。

    // 锚点必须落在拾取射线上。PickingService::pickTerrain 是"先与椭球求交、
    // 再按该经纬度的地形高沿局部垂直抬起"，返回点并不在射线上；而锚点跟手的
    // 整套数学（抓取球、锚点钉合）都假定锚点就在起始射线上，这段落差会被第一
    // 个 move 当成手指位移一次性补掉——真机实测起手跳变 227~471px，海拔越低
    // 越大。方向取射线∩抓取球（miss 取最近接近点），于是 t=0 时锚点投影严格
    // 等于手指像素。
    bool trueHit = false;
    if (!pointOnGrabSphere(ray, grabbedPoint, trueHit)) {
        hasGrabbedPoint_ = false;
        return false;
    }
    grabbedPoint_ = grabbedPoint;
    grabbedNormal_ = grabbedPoint.normalized();
    hasGrabbedPoint_ = true;
    return true;
}

void FreeGlobeController::applyAnchorDrag(float xPixels, float yPixels,
                                              double timestamp) {
    glm::dquat delta{1.0, 0.0, 0.0, 0.0};
    bool haveDelta = false;
    bool regrabAfterApply = false;

    // move 期锚定在抓取球面（半径=抓取点半径），不重 pick 地形：from/to 同
    // 球面才能一次旋转把锚点精确放回指下。重 pick 地形时，指下地形高≠抓取
    // 点高，法线对齐后锚点投影偏离手指（起伏越大/视角越斜越明显）＝不跟手。
    if (hasGrabbedPoint_) {
        const AnchorSolveResult solve =
            solveAnchorRotation(grabbedNormal_, xPixels, yPixels);
        if (solve.valid) {
            const double w = anchorExactWeight(solve.conditioning);
            if (w >= 1.0) {
                if (solve.degenerate) {
                    return;  // 良态区对齐：无事发生（不更新惯性/时间戳）
                }
                delta = solve.delta;
            } else {
                // 病态区（掠射/球缘外）：精确解增益 ∝1/c 爆炸，连续混入
                // 转台旋转；w→0 时退化为纯转台（球外拖拽=旧 spin 手感）。
                // 应用后整点重取锚点（见下），欠账不累积，条件数恢复时
                // 没有补偿跳变——这替代了旧的"miss 即 latch 到抬手"。
                delta = glm::slerp(spinTurntableDelta(xPixels, yPixels),
                                   solve.delta, w);
                regrabAfterApply = true;
            }
            haveDelta = true;
        }
    }

    if (!haveDelta) {
        // 极端退化（球心在射线后方/射线穿地心）或起手就没抓到点：纯转台。
        const double dx = static_cast<double>(xPixels) - dragLastX_;
        const double dy = static_cast<double>(yPixels) - dragLastY_;
        if (std::abs(dx) < 1e-6 && std::abs(dy) < 1e-6) {
            return;
        }
        delta = spinTurntableDelta(xPixels, yPixels);
    }

    camera_ops::rotateAboutOrigin(*camera_, delta);
    {
        const glm::dvec3 anchorWorld = grabbedPoint_.raw();
        clampNow(hasGrabbedPoint_ ? &anchorWorld : nullptr);
    }

    // 退化区不变量：锚点良态区整段不可变；仅退化区（w<1）在应用旋转后被
    // 整点重取为"当前手指下抓取球上的点"（半径不变，绝不重 pick 地形）。
    // 永不渐近混合——混合出的锚点不属于任何真实几何，会积欠账。
    if (regrabAfterApply && hasGrabbedPoint_) {
        const Ray ray = camera_->getPickRay(
            static_cast<double>(xPixels),
            static_cast<double>(yPixels),
            static_cast<double>(viewportWidth_),
            static_cast<double>(viewportHeight_));
        Vec3 regrabbed;
        bool trueHit = false;
        if (pointOnGrabSphere(ray, regrabbed, trueHit)) {
            grabbedPoint_ = regrabbed;
            grabbedNormal_ = regrabbed.normalized();
        }
    }

    // 惯性采样（使用事件时间戳，而非渲染时钟）：只记录最近 ≤3 个相邻样本，
    // 松手时在 onDragEnd 按 iOS 权重 0.6/0.35/0.05 合成（契约 1.4）。spin 与
    // anchor 共用同一通道，故隔着地平线甩一下也能顺滑滑行。
    const double angle = glm::angle(delta);
    const double dt = timestamp - lastDragTimestamp_;
    if (angle > 1e-9 && dt > 0.0 && dt < 0.25) {
        DragVelocitySample sample;
        sample.dt = dt;
        sample.axis = glm::axis(delta);
        sample.rate = std::min(angle / dt,
                               kMaxInertiaAngularVelocityRadPerSec);
        dragVelocitySamples_.push_back(sample);
        if (dragVelocitySamples_.size() > 3) {
            dragVelocitySamples_.erase(dragVelocitySamples_.begin());
        }
    }
    lastDragTimestamp_ = timestamp;
}

} // namespace earth_engine
