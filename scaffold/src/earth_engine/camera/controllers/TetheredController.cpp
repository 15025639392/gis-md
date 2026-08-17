#include "TetheredController.h"

#include "../CameraConstraintSolver.h"
#include "../CameraPose.h"
#include "../../core/math/Vec3.h"
#include "../../scene/Camera.h"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

namespace earth_engine {

namespace {

// 参考系"动了没有"的判据。位置阈值取得很小(毫米级):载体缓慢移动时也得继续
// 出帧,否则画面会一顿一顿。姿态阈值同理取到 1e-9 量级的方向余弦差。
constexpr double kFrameMoveEpsilonMeters = 1e-3;
constexpr double kFrameRotateEpsilon = 1e-9;

// 拖拽增益:满屏横移转一整圈的一半(π),竖移转 ±90°。按视口归一 ⇒ 设备无关
// (与 Free 侧 kPinchTiltRadiansPerPixel 同一取向,别再写成每物理像素)。
constexpr double kDragFullWidthYawRadians = glm::pi<double>();
constexpr double kDragFullHeightPitchRadians = glm::pi<double>() * 0.5;

// 捏合 jerk 限幅,与 Free 侧同值。
constexpr double kTouchJerkLimit = 0.3;

}  // namespace

TetheredController::TetheredController(Camera* camera,
                                       CameraConstraintSolver* solver)
    : camera_(camera), solver_(solver) {
    (void)solver_;  // 约束由编排层的帧末哨兵施加,本控制器不自己钳(见头文件)
}

void TetheredController::setFrame(ViewpointFrame frame) {
    frame_ = std::move(frame);
    hasPreviousFrame_ = false;
    frameMoving_ = false;
}

void TetheredController::setLocalOrientation(double headingRadians,
                                             double pitchRadians,
                                             double rollRadians) {
    heading_ = headingRadians;
    pitch_ = std::clamp(pitchRadians, -kMaxPitchRadians, kMaxPitchRadians);
    roll_ = rollRadians;
}

void TetheredController::setRange(double rangeMeters) {
    range_ = std::max(kMinRangeMeters, rangeMeters);
}

void TetheredController::setViewport(int widthPixels, int heightPixels) {
    viewportWidth_ = std::max(1, widthPixels);
    viewportHeight_ = std::max(1, heightPixels);
}

bool TetheredController::resolveFrame(glm::dvec3& outOrigin,
                                      glm::dmat3& outBasis) const {
    glm::dvec3 origin{0.0};
    if (frame_.originProvider) {
        if (!frame_.originProvider(origin)) {
            return false;  // 目标暂不可用:调用方保持上帧,不回落世界系
        }
    }
    outOrigin = origin;
    // orientationProvider 空 ⇒ 用原点处的地理 ENU(跟车但保持北上)。
    if (frame_.orientationProvider) {
        glm::dmat3 provided{1.0};
        if (frame_.orientationProvider(provided)) {
            outBasis = provided;
            return true;
        }
    }
    outBasis = CameraPose::enuFrameAt(origin);
    return true;
}

void TetheredController::applyPose(const glm::dvec3& origin,
                                   const glm::dmat3& basis) {
    const CameraPose pose =
        CameraPose::fromFrame(origin, basis, heading_, pitch_, roll_, range_);
    camera_->setView(Vec3(pose.eye), Vec3(pose.direction), Vec3(pose.up));
}

void TetheredController::alignToCurrentPose(const glm::dvec3& origin,
                                            const glm::dmat3& basis) {
    const glm::dvec3 eye = camera_->position().raw();
    const glm::dvec3 offset = eye - origin;
    const double distance = glm::length(offset);

    CameraPose aligned;
    aligned.eye = eye;
    if (distance > 1e-9) {
        // ⚠️ **重新对准载体**。系留的真值 (localHPR, range) 是 orbit 表述:
        // `fromFrame` 由 `eye = origin − direction·range` 反推位置,只有在视线
        // 正对原点时才与 `toFrame` 自洽。相机原本没看着载体时,位置与朝向**只能
        // 保一个**——判据说的是「跳变 < 1e-6 **米**」,所以保位置。
        // 表现:从 Free 切进来时若原本没看着载体,视线会一次性转向它;位置不动。
        aligned.direction = -offset / distance;
    } else {
        aligned.direction = camera_->direction().raw();  // range≈0(座舱视角)
    }
    // roll 尽量保留:把当前 up 对新视线做正交化,而不是直接置零 —— 地平线倾角
    // 是观感上最刺眼的量,能留就留。
    glm::dvec3 up = camera_->up().raw();
    up -= aligned.direction * glm::dot(up, aligned.direction);
    if (glm::length(up) < 1e-9) {
        up = basis[2] - aligned.direction * glm::dot(basis[2], aligned.direction);
    }
    aligned.up = glm::length(up) > 1e-9 ? glm::normalize(up) : basis[2];

    aligned.toFrame(origin, basis, heading_, pitch_, roll_, range_);
    pitch_ = std::clamp(pitch_, -kMaxPitchRadians, kMaxPitchRadians);
}

void TetheredController::onActivate() {
    glm::dvec3 origin{0.0};
    glm::dmat3 basis{1.0};
    if (!resolveFrame(origin, basis)) {
        // 参考系还没准备好:先不动相机,等第一次成功 tick 再对齐。此时
        // frameResolved_ 保持 false,调用方能看出"还没真正接管上"。
        frameResolved_ = false;
        return;
    }

    alignToCurrentPose(origin, basis);

    frameResolved_ = true;
    hasPreviousFrame_ = true;
    previousOrigin_ = origin;
    previousBasis_ = basis;
    // 接管当帧不该被判成"载体在动"——那点差异来自换算不是来自载体。
    frameMoving_ = false;

    dragging_ = false;
    pinching_ = false;
    pinchAppliedScaleLog_ = 0.0;
}

void TetheredController::onDeactivate() {
    dragging_ = false;
    pinching_ = false;
    pinchAppliedScaleLog_ = 0.0;
    frameMoving_ = false;
    hasPreviousFrame_ = false;
}

void TetheredController::tick(double deltaSeconds) {
    (void)deltaSeconds;  // 系留没有自走动画:位姿完全由参考系与真值决定
    glm::dvec3 origin{0.0};
    glm::dmat3 basis{1.0};
    if (!resolveFrame(origin, basis)) {
        frameResolved_ = false;
        frameMoving_ = false;
        return;  // 保持上帧位姿
    }

    // 首次解算成功(接管时 provider 还没准备好的情况)——补做一次对齐。
    if (!frameResolved_) {
        alignToCurrentPose(origin, basis);
        frameResolved_ = true;
    }

    // 参考系动了没有 —— isAnimating() 的依据。恒 true 会让静止的系留相机永远
    // 不空闲(按需渲染彻底失效);恒 false 则载体一动画面就停在半路。
    frameMoving_ = false;
    if (hasPreviousFrame_) {
        if (glm::length(origin - previousOrigin_) > kFrameMoveEpsilonMeters) {
            frameMoving_ = true;
        } else {
            for (int c = 0; c < 3 && !frameMoving_; ++c) {
                if (glm::length(basis[c] - previousBasis_[c]) >
                    kFrameRotateEpsilon) {
                    frameMoving_ = true;
                }
            }
        }
    }
    hasPreviousFrame_ = true;
    previousOrigin_ = origin;
    previousBasis_ = basis;

    applyPose(origin, basis);
}

// ============================================================
// 触摸手势(语义与 Free 完全不同,见头文件)
// ============================================================

void TetheredController::onDragStart(float xPixels, float yPixels,
                                     double timestamp) {
    (void)timestamp;  // 系留拖拽不种惯性:载体在动,惯性滑行会和跟随打架
    dragging_ = true;
    dragLastX_ = xPixels;
    dragLastY_ = yPixels;
}

void TetheredController::onDragMove(float xPixels, float yPixels,
                                    double timestamp) {
    (void)timestamp;
    if (!dragging_) return;

    const double dx = static_cast<double>(xPixels - dragLastX_);
    const double dy = static_cast<double>(yPixels - dragLastY_);
    dragLastX_ = xPixels;
    dragLastY_ = yPixels;

    // 手指右移 ⇒ 载体应该在屏幕上向右走 ⇒ 相机绕载体逆时针公转 ⇒ heading 减小。
    // 与 Free 侧"手指右移世界右转"同一取向。⚠️ 增益与手感须真机验,host 只能
    // 钉住符号与不变量(range 不变、载体保持在视线上)。
    heading_ -= dx * kDragFullWidthYawRadians /
                static_cast<double>(std::max(1, viewportWidth_));
    // 手指下移 ⇒ 视角往下压。
    pitch_ = std::clamp(pitch_ - dy * kDragFullHeightPitchRadians /
                                     static_cast<double>(
                                         std::max(1, viewportHeight_)),
                        -kMaxPitchRadians, kMaxPitchRadians);
}

void TetheredController::onDragEnd() { dragging_ = false; }

void TetheredController::onPinchGesture(const PinchInput& input) {
    dragging_ = false;
    if (!pinching_) {
        pinching_ = true;
        pinchAppliedScaleLog_ = 0.0;
    }

    // 绝对量 + jerk 限幅,与 Free 侧同构:事件被合并/丢弃不产生累积漂移。
    const double requestedLog =
        std::log(std::max(static_cast<double>(input.scaleFromStart), 1e-6));
    const double jerkStepLimit = std::log(1.0 + kTouchJerkLimit);
    const double newAppliedLog =
        pinchAppliedScaleLog_ +
        std::clamp(requestedLog - pinchAppliedScaleLog_,
                   std::log(1.0 - kTouchJerkLimit), jerkStepLimit);
    const double stepScale = std::exp(newAppliedLog - pinchAppliedScaleLog_);
    pinchAppliedScaleLog_ = newAppliedLog;

    // 捏开(scale>1)= 拉近 ⇒ range 变小。
    setRange(range_ / stepScale);
}

void TetheredController::onPinchEnd() {
    pinching_ = false;
    pinchAppliedScaleLog_ = 0.0;
}

} // namespace earth_engine
