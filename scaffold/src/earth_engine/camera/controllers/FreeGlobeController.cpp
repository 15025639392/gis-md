#include "FreeGlobeController.h"

#include "../CameraConstraintSolver.h"
#include "../CameraPoseOps.h"
#include "../../core/geodesy/Cartographic.h"
#include "../../core/geodesy/Ellipsoid.h"
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
constexpr double kInertiaDampingPerSecond = 3.0;
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
// Pitch 增益按视口高度归一：满屏竖移 = 0.9 rad（等价旧 0.0015 rad/px 在
// 600px 视口下的标定），设备/分辨率无关——旧的每物理像素定义在 3.5x 手机
// 上比 1.5x 平板快 2.3 倍。
constexpr double kPinchTiltFullHeightRadians = 0.9;
constexpr double kPinchTiltMaxStepRadians = 0.08;
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

// 高空缩放回中：拉远到高空时把"视线偏离地心"的夹角 θ 收敛到 0，让球心回到
// 屏幕中心。硬约束事件原子、软约束时间松弛：手势/滑行期只按 zoom-out 对数
// 步长"充值"预算（不动相机——回中旋转会推开刚钉好的锚点，与 pin 互相拉扯），
// 松手后 tick() 按指数节奏消费预算，球心在 ~0.3-0.5s 内自然沉降回中。
// 由此手势期 viewLineMissDistance 成为全海拔精确不变量。权重随海拔在
// [start, full] 间平滑爬升，低空(城市/地形视角)完全不介入。
constexpr double kRecenterStartAltitudeMeters = 1.5e6;  // 低于此海拔不回中
constexpr double kRecenterFullAltitudeMeters = 8.0e6;   // 高于此海拔全权重
constexpr double kRecenterGainPerLogStep = 2.5;  // 预算充值速率 / 单位 ln(距离)
constexpr double kRecenterSettleRatePerSecond = 6.0;  // 松手后 θ 指数收敛速率

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
    // 拖拽=用户显式抓世界重定位，残留的回中欠账此后再沉降会像无因漂移。
    recenterBudgetRadians_ = 0.0;
    lastDragTimestamp_ = timestamp;
}

void FreeGlobeController::onDragMove(float xPixels, float yPixels,
                                         double timestamp) {
    if (!dragging_) return;

    applyAnchorDrag(xPixels, yPixels, timestamp);

    dragLastX_ = xPixels;
    dragLastY_ = yPixels;
}

void FreeGlobeController::onDragEnd() {
    if (!dragging_) return;
    dragging_ = false;
    hasGrabbedPoint_ = false;
    // 惯性参数由最后一次 applyAnchorDrag 设置
}

void FreeGlobeController::onPinchGesture(float scale,
                                             float centerX,
                                             float centerY,
                                             float rotationRadians,
                                             float centerDeltaX,
                                             float centerDeltaY,
                                             double timestamp) {
    // 旧契约薄适配器：每事件增量累积成绝对量转发新接口。centerDeltaX/Y
    // 不再消费——倾斜由 InputManager 的 Pitch latch 走新契约表达。
    (void)centerDeltaX;
    (void)centerDeltaY;
    if (scale <= 0.0f) return;
    if (!pinching_) {
        adapterScaleLog_ = 0.0;
        adapterTwistRadians_ = 0.0;
    }
    adapterScaleLog_ += std::log(static_cast<double>(scale));
    adapterTwistRadians_ += static_cast<double>(rotationRadians);

    PinchInput input;
    input.scaleFromStart = static_cast<float>(std::exp(adapterScaleLog_));
    input.twistFromStartRadians = static_cast<float>(adapterTwistRadians_);
    input.centroidX = centerX;
    input.centroidY = centerY;
    input.mode = PinchMode::Manipulate;
    input.timestamp = timestamp;
    onPinchGesture(input);
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
        lastPinchTimestamp_ = input.timestamp;
        pinchAppliedScaleLog_ = 0.0;
        pinchAppliedTwistRadians_ = 0.0;
        pinchPanAngularVelocity_ = 0.0;
        pinchActiveMode_ = PinchMode::Undecided;
        pitchAppliedRadians_ = 0.0;
        pitchBaselineY_ = input.centroidY;
        pitchPinX_ = input.centroidX;
        pitchPinY_ = input.centroidY;
        pinchAnchorScreenX_ = input.centroidX;
        pinchAnchorScreenY_ = input.centroidY;
        lastPinchCentroidX_ = input.centroidX;
        lastPinchCentroidY_ = input.centroidY;

        hasPinchAnchor_ =
            tryAcquirePinchAnchor(input.centroidX, input.centroidY);
        platformLog(LogLevel::Info, "CameraCtrl",
            "pinchStart hasAnchor=%d center=(%.0f,%.0f)",
            hasPinchAnchor_, input.centroidX, input.centroidY);
    }

    inertiaAngularVelocity_ = 0.0;

    // mode 迁移（latch 在 InputManager，一段手势最多一次）：
    // Undecided→Manipulate 无需特殊处理——pin 是状态求解而非增量累加，pin
    // 目标切到当前质心后，窗口期积压的质心行程被一次精确补齐（上界=latch
    // 阈值 8dp，肉眼不可见），不丢量、不需要回滚。
    // Undecided→Pitch：以 latch 时刻的质心为 pitch 基线与钉合像素。
    if (input.mode != pinchActiveMode_) {
        if (input.mode == PinchMode::Pitch) {
            pitchBaselineY_ = input.centroidY;
            pitchPinX_ = input.centroidX;
            pitchPinY_ = input.centroidY;
            pitchAppliedRadians_ = 0.0;
        }
        pinchActiveMode_ = input.mode;
    }

    // Jerk 限幅（绝对量状态表述）：本事件增量 = 请求累计 − 已施加累计，
    // 夹到 ±ln(1.3)；已施加值对请求值的落后量夹到 ±kMaxPinchScaleResidualLog，
    // 防事件流长时间单向饱和攒出一段松手仍在自走的"欠账"。语义等价旧的
    // 残差补回机制，但绝对量表述在事件被合并/丢弃时不漂。
    const double jerkStepLimit = std::log(1.0 + kTouchJerkLimit);
    const double requestedLog =
        std::log(std::max(static_cast<double>(input.scaleFromStart), 1e-6));
    double newAppliedLog = pinchAppliedScaleLog_ +
        std::clamp(requestedLog - pinchAppliedScaleLog_,
                   std::log(1.0 - kTouchJerkLimit), jerkStepLimit);
    newAppliedLog = std::clamp(newAppliedLog,
                               requestedLog - kMaxPinchScaleResidualLog,
                               requestedLog + kMaxPinchScaleResidualLog);
    const double stepScale = std::exp(newAppliedLog - pinchAppliedScaleLog_);
    pinchAppliedScaleLog_ = newAppliedLog;

    // 无锚起手（球外/pick 全 miss）后每事件重试获取，成功即转入有锚分支。
    if (!hasPinchAnchor_) {
        hasPinchAnchor_ =
            tryAcquirePinchAnchor(input.centroidX, input.centroidY);
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

        // ② twist：绕锚点、以锚点法线为轴。锚点在轴上 ⇒ 精确保锚。
        // 无阈值——死区会吞掉刻意的慢速拧动（steppy），噪声由 InputManager
        // 的 latch 窗口与传感自身平滑兜底。
        const double twistStep = static_cast<double>(
            input.twistFromStartRadians) - pinchAppliedTwistRadians_;
        pinchAppliedTwistRadians_ =
            static_cast<double>(input.twistFromStartRadians);
        if (std::abs(twistStep) > 0.0) {
            camera_ops::rotateAboutPoint(*camera_, anchorWorld,
                                         pinchAnchorNormal_.raw(), twistStep);
        }

        // ③ pitch（仅 Pitch 模式）：质心 Y 相对基线的绝对映射，绕锚点
        // 转相机竖直面（精确保锚）。反 wind-up：只把实际施加的量计入
        // pitchAppliedRadians_，被守卫拒绝时重取基线——到达俯仰界后反向
        // 立即响应，零死区离合。
        if (pinchActiveMode_ == PinchMode::Pitch) {
            const double radPerPixel = kPinchTiltFullHeightRadians /
                static_cast<double>(std::max(1, viewportHeight_));
            const double desired = -radPerPixel *
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
                        static_cast<float>(pitchAppliedRadians_ / radPerPixel);
                }
            }
        }

        // ④ pin：唯一产生横向世界运动的通道。Manipulate 钉当前质心（刚性
        // pan 内建），Undecided 钉起手质心，Pitch 钉 latch 质心。
        float pinTargetX = input.centroidX;
        float pinTargetY = input.centroidY;
        if (pinchActiveMode_ == PinchMode::Undecided) {
            pinTargetX = pinchAnchorScreenX_;
            pinTargetY = pinchAnchorScreenY_;
        } else if (pinchActiveMode_ == PinchMode::Pitch) {
            pinTargetX = pitchPinX_;
            pinTargetY = pitchPinY_;
        }
        const glm::dquat pinDelta = applyPinchPin(pinTargetX, pinTargetY);

        // 双指 pan 惯性累积（EMA，与 zoom 惯性同构：静止/纯缩放帧采样 0，
        // 速率自然衰减向 0，只有真正在平移才留下动量）。Pitch/Undecided 的
        // pin 目标静止 → delta≈identity → 自动不积。松手时种进与单指共用
        // 的惯性通道（onPinchEnd）。
        {
            const double dt = input.timestamp - lastPinchTimestamp_;
            if (dt > 0.0 && dt < 0.25) {
                const double angle = glm::angle(pinDelta);
                const double instVelocity = angle > 1e-9
                    ? std::min(angle / dt, kMaxInertiaAngularVelocityRadPerSec)
                    : 0.0;
                if (angle > 1e-9) {
                    pinchPanAxis_ = glm::axis(pinDelta);
                }
                pinchPanAngularVelocity_ =
                    pinchPanAngularVelocity_ * (1.0 - kVelocitySmoothing) +
                    instVelocity * kVelocitySmoothing;
            }
        }

        if (stepScale < 1.0) {
            accrueRecenterBudget(-std::log(stepScale));
        }
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
        if (stepScale < 1.0) {
            accrueRecenterBudget(-std::log(stepScale));
        }
    }

    lastPinchCentroidX_ = input.centroidX;
    lastPinchCentroidY_ = input.centroidY;
    lastPinchTimestamp_ = input.timestamp;
}

void FreeGlobeController::onPinchEnd() {
    pinching_ = false;
    adapterScaleLog_ = 0.0;
    adapterTwistRadians_ = 0.0;
    // 双指 pan 惯性：把手势期累积的 pin 角速度种进与单指拖拽共用的惯性
    // 通道（tick() 统一衰减；量太小会立即跌破 1e-4 地板自然停）。
    inertiaAngularVelocity_ = pinchPanAngularVelocity_;
    inertiaAxis_ = pinchPanAxis_;
    pinchPanAngularVelocity_ = 0.0;
    // 松手时若刚才在缩放且留有足够动量，启动 zoom 惯性滑行（锚点仍需保留以
    // 沿视线朝它 dolly）。否则清零。
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
    adapterScaleLog_ = 0.0;
    adapterTwistRadians_ = 0.0;
    pinchPanAngularVelocity_ = 0.0;
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
        glm::dquat delta = glm::angleAxis(angle, inertiaAxis_);
        camera_ops::rotateAboutOrigin(*camera_, delta);
        clampNow(nullptr);
        inertiaAngularVelocity_ *= std::exp(-kInertiaDampingPerSecond * deltaSeconds);
        // 掉到阈值以下就归零。指数衰减永远够不到 0,留着那个小尾巴的话
        // 「是否还在动」这个问题就永远答"是"(zoom 惯性早就是这么自清的,
        // pan 这条一直缺)。
        if (inertiaAngularVelocity_ <= kMinInertiaAngularVelocity) {
            inertiaAngularVelocity_ = 0.0;
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
            // 拉远方向的惯性滑行（rate<0 = 距离增大）继续充值回中预算。
            if (zoomInertiaLogRate_ < 0.0) {
                accrueRecenterBudget(-zoomInertiaLogRate_ * deltaSeconds);
            }
        }
        zoomInertiaLogRate_ *=
            std::exp(-kZoomInertiaDampingPerSecond * deltaSeconds);
        if (std::abs(zoomInertiaLogRate_) < kMinZoomInertiaLogRate) {
            hasZoomInertia_ = false;
            zoomInertiaLogRate_ = 0.0;
        }
    }

    // 高空回中预算消费：仅在无活动手势时沉降（手势期严格正交——回中旋转
    // 会推开刚钉好的锚点）。
    if (!dragging_ && !pinching_) {
        consumeRecenterBudget(deltaSeconds);
    }
}

void FreeGlobeController::clearPanInertia() {
    inertiaAngularVelocity_ = 0.0;
}

void FreeGlobeController::clearGlideInertia() {
    clearPanInertia();
    hasZoomInertia_ = false;
    zoomInertiaLogRate_ = 0.0;
}

void FreeGlobeController::clearAllInertia() {
    clearGlideInertia();
    recenterBudgetRadians_ = 0.0;
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

void FreeGlobeController::accrueRecenterBudget(double zoomOutLogStep) {
    if (zoomOutLogStep <= 0.0) return;

    const glm::dvec3 eye = camera_->position().raw();
    const double eyeRadius = glm::length(eye);
    const double altitude = eyeRadius - kEarthRadiusMeters;
    if (altitude <= kRecenterStartAltitudeMeters) return;

    const glm::dvec3 toCenter = -eye / eyeRadius;
    const glm::dvec3 dir = glm::normalize(camera_->direction().raw());
    const double cosTheta = std::clamp(glm::dot(dir, toCenter), -1.0, 1.0);
    const double theta = std::acos(cosTheta);
    if (theta < 1e-6) return;

    double w = (altitude - kRecenterStartAltitudeMeters) /
               (kRecenterFullAltitudeMeters - kRecenterStartAltitudeMeters);
    w = std::clamp(w, 0.0, 1.0);
    w = w * w * (3.0 - 2.0 * w);  // smoothstep：介入边界无手感突变

    recenterBudgetRadians_ +=
        w * kRecenterGainPerLogStep * zoomOutLogStep * theta;
    // 预算封顶到当前 θ：θ 收敛到 0 后多余欠账没有意义。
    recenterBudgetRadians_ = std::min(recenterBudgetRadians_, theta);
}

void FreeGlobeController::consumeRecenterBudget(double deltaSeconds) {
    if (recenterBudgetRadians_ <= 1e-9 || deltaSeconds <= 0.0) {
        return;
    }

    const glm::dvec3 eye = camera_->position().raw();
    const double eyeRadius = glm::length(eye);
    if (eyeRadius < 1e-6) return;
    const glm::dvec3 toCenter = -eye / eyeRadius;
    const glm::dvec3 dir = glm::normalize(camera_->direction().raw());
    const double cosTheta = std::clamp(glm::dot(dir, toCenter), -1.0, 1.0);
    const double theta = std::acos(cosTheta);
    if (theta < 1e-6) {
        recenterBudgetRadians_ = 0.0;
        return;
    }

    glm::dvec3 axis = glm::cross(dir, toCenter);
    const double axisLength = glm::length(axis);
    if (axisLength < 1e-12) {
        recenterBudgetRadians_ = 0.0;
        return;
    }
    axis /= axisLength;

    const double step = std::min(
        recenterBudgetRadians_,
        theta * (1.0 - std::exp(-kRecenterSettleRatePerSecond * deltaSeconds)));
    // 绕相机自身位置旋转：eye 不动，仅视线向地心方向收敛 → 球心向屏幕中心靠。
    camera_ops::rotateAboutPoint(*camera_, eye, axis, step);
    recenterBudgetRadians_ -= step;
    if (recenterBudgetRadians_ < 1e-9) {
        recenterBudgetRadians_ = 0.0;
    }
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

    // 更新惯性（使用事件时间戳，而非渲染时钟）；spin 与 anchor 共用同一通道，
    // 故隔着地平线甩一下也能顺滑滑行。
    const double angle = glm::angle(delta);
    double dt = timestamp - lastDragTimestamp_;
    if (angle > 1e-9 && dt > 0.0 && dt < 0.25) {
        double instantaneousVelocity = std::min(angle / dt,
                                                kMaxInertiaAngularVelocityRadPerSec);
        inertiaAxis_ = glm::axis(delta);
        inertiaAngularVelocity_ =
            inertiaAngularVelocity_ * (1.0 - kVelocitySmoothing) +
            instantaneousVelocity * kVelocitySmoothing;
    }
    lastDragTimestamp_ = timestamp;
}

} // namespace earth_engine
