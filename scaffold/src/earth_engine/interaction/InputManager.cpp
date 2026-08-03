#include "InputManager.h"
#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

// ---- mode latch 阈值（dp，事件的 devicePixelRatio 换算物理像素）----
// 参考 MapLibre："起手快照 + 单次 latch"。判据全部是"当前 vs 起手基准"
// 而非 vs 上一帧——未 latch 前依据不变的起点重判，一旦 latch 整段手势不变。
constexpr float kLatchSpreadLogThreshold = 0.05f;   // |ln(spread/spread0)|
constexpr float kLatchTwistThresholdRadians = 0.05f;
constexpr float kLatchCentroidXDp = 8.0f;           // 质心水平位移
constexpr float kPitchFingerMoveDp = 2.0f;          // 每指最小有效位移
constexpr float kPitchVerticalDominance = 1.5f;     // |dy| 须 > 1.5|dx|
constexpr double kLatchTimeoutSeconds = 0.15;       // 超时兜底 Manipulate

float unwrapAngle(float angle, float reference) {
    constexpr float kPi = 3.14159265358979323846f;
    while (angle >= reference + kPi) angle -= 2.0f * kPi;
    while (angle < reference - kPi) angle += 2.0f * kPi;
    return angle;
}

} // namespace

void InputManager::processPinchWithPointerPair(InputEvent& event) {
    PinchSession& s = pinchSession_;
    const float dx = event.pointer1X - event.pointer0X;
    const float dy = event.pointer1Y - event.pointer0Y;
    const float spread = std::sqrt(dx * dx + dy * dy);
    const float angleRaw = std::atan2(dy, dx);
    const float centroidX = (event.pointer0X + event.pointer1X) * 0.5f;
    const float centroidY = (event.pointer0Y + event.pointer1Y) * 0.5f;

    if (!s.baselineValid) {
        // 首个携带 pointer pair 的事件建立起手基准（PinchStart 只带质心，
        // 两指坐标从首个 PinchMove 才有）。
        s.baselineValid = true;
        s.spread0 = std::max(spread, 1.0f);
        s.angle0 = angleRaw;
        s.centroid0X = centroidX;
        s.centroid0Y = centroidY;
        s.p0StartX = event.pointer0X;
        s.p0StartY = event.pointer0Y;
        s.p1StartX = event.pointer1X;
        s.p1StartY = event.pointer1Y;
        s.t0 = event.timestamp;
        s.prevAngleRaw = angleRaw;
        s.angleUnwrapped = angleRaw;
        s.mode = InputEvent::PinchMode::Undecided;
    } else {
        s.angleUnwrapped += unwrapAngle(angleRaw, s.prevAngleRaw) - s.prevAngleRaw;
        s.prevAngleRaw = angleRaw;
    }

    const float twistFromStart = s.angleUnwrapped - s.angle0;
    const float spreadLog = std::log(std::max(spread, 1.0f) / s.spread0);

    if (s.mode == InputEvent::PinchMode::Undecided) {
        const float dpr = std::max(event.devicePixelRatio, 0.5f);
        const float v0x = event.pointer0X - s.p0StartX;
        const float v0y = event.pointer0Y - s.p0StartY;
        const float v1x = event.pointer1X - s.p1StartX;
        const float v1y = event.pointer1Y - s.p1StartY;

        // Manipulate 优先：pitch 手势按定义不改 spread/连线角/质心横位。
        // 错判偏向"能平移"一侧（代价小），且超时兜底也是 Manipulate。
        if (std::abs(spreadLog) > kLatchSpreadLogThreshold ||
            std::abs(twistFromStart) > kLatchTwistThresholdRadians ||
            std::abs(centroidX - s.centroid0X) > kLatchCentroidXDp * dpr) {
            s.mode = InputEvent::PinchMode::Manipulate;
        } else if (std::abs(v0y) > kPitchFingerMoveDp * dpr &&
                   std::abs(v1y) > kPitchFingerMoveDp * dpr) {
            // 两指都动了才能判定方向性：平行竖移且同向 → Pitch，否则排除。
            const bool sameDirection = (v0y > 0.0f) == (v1y > 0.0f);
            const bool bothVertical =
                std::abs(v0y) > kPitchVerticalDominance * std::abs(v0x) &&
                std::abs(v1y) > kPitchVerticalDominance * std::abs(v1x);
            s.mode = (sameDirection && bothVertical)
                ? InputEvent::PinchMode::Pitch
                : InputEvent::PinchMode::Manipulate;
        } else if (event.timestamp - s.t0 > kLatchTimeoutSeconds) {
            s.mode = InputEvent::PinchMode::Manipulate;
        }
    }

    event.pinchMode = s.mode;
    event.pinchScaleFromStart = std::exp(spreadLog);
    event.twistFromStartRadians = twistFromStart;
}

void InputManager::process(const InputEvent& event) {
    if (!callback_) return;

    if (event.type == InputEvent::Type::Cancel) {
        cancelActiveGesture();
        return;
    }

    // ---- Pinch 事件：维护双指会话（派生量 + mode latch）后转发 ----
    if (event.isPinchEvent()) {
        switch (event.type) {
            case InputEvent::Type::PinchStart:
                cancelActiveGesture();
                state_ = State::TwoFinger;
                pinchActive_ = true;
                suppressClick_ = true;
                pinchSession_ = PinchSession{};
                {
                    InputEvent start = event;
                    if (start.hasPointerPair) {
                        processPinchWithPointerPair(start);
                    }
                    callback_(Gesture::PinchStart, start);
                }
                break;
            case InputEvent::Type::PinchMove: {
                if (!pinchActive_) {
                    state_ = State::TwoFinger;
                    pinchActive_ = true;
                    suppressClick_ = true;
                    pinchSession_ = PinchSession{};
                    InputEvent start = event;
                    start.type = InputEvent::Type::PinchStart;
                    start.pinchScale = 1.0f;
                    start.rotationRadians = 0.0f;
                    start.centerDeltaX = 0.0f;
                    start.centerDeltaY = 0.0f;
                    if (start.hasPointerPair) {
                        processPinchWithPointerPair(start);
                    }
                    callback_(Gesture::PinchStart, start);
                }
                InputEvent move = event;
                if (move.hasPointerPair) {
                    processPinchWithPointerPair(move);
                }
                callback_(Gesture::PinchMove, move);
                break;
            }
            case InputEvent::Type::PinchEnd:
                if (pinchActive_) {
                    callback_(Gesture::PinchEnd, event);
                }
                state_ = State::Idle;
                pinchActive_ = false;
                suppressClick_ = true;
                pinchSession_ = PinchSession{};
                break;
            default:
                break;
        }
        return;
    }

    // ---- 指针事件 ----
    switch (event.type) {
        case InputEvent::Type::PointerDown:
            cancelActiveGesture();
            state_ = State::OneFingerPending;
            suppressClick_ = false;
            trackStartX_ = event.screenX;
            trackStartY_ = event.screenY;
            trackLastX_ = event.screenX;
            trackLastY_ = event.screenY;
            break;

        case InputEvent::Type::PointerMove:
            if (state_ != State::OneFingerPending &&
                state_ != State::OneFingerDrag) {
                break;
            }

            if (state_ == State::OneFingerPending) {
                float dx = event.screenX - trackStartX_;
                float dy = event.screenY - trackStartY_;
                if (std::sqrt(dx * dx + dy * dy) >= dragThreshold_) {
                    state_ = State::OneFingerDrag;
                    // DragStart 使用起始位置
                    InputEvent dragStartEvent = event;
                    dragStartEvent.type = InputEvent::Type::PointerDown;
                    dragStartEvent.screenX = trackStartX_;
                    dragStartEvent.screenY = trackStartY_;
                    callback_(Gesture::DragStart, dragStartEvent);
                }
            }

            if (state_ == State::OneFingerDrag) {
                callback_(Gesture::DragMove, event);
                trackLastX_ = event.screenX;
                trackLastY_ = event.screenY;
            }
            break;

        case InputEvent::Type::PointerUp: {
            finishPointerGesture(event);
            break;
        }

        default:
            break;
    }
}

void InputManager::reset() {
    state_ = State::Idle;
    pinchActive_ = false;
    suppressClick_ = false;
    lastClickTime_ = -1.0;
    pinchSession_ = PinchSession{};
}

void InputManager::finishPointerGesture(const InputEvent& event) {
    if (state_ == State::OneFingerDrag) {
        callback_(Gesture::DragEnd, event);
        state_ = State::Idle;
        suppressClick_ = true;
        return;
    }

    if (state_ != State::OneFingerPending) {
        state_ = State::Idle;
        return;
    }

    state_ = State::Idle;
    if (suppressClick_) {
        suppressClick_ = false;
        return;
    }

    bool isDouble = false;
    if (lastClickTime_ > 0.0 &&
        event.timestamp - lastClickTime_ <= doubleClickInterval_) {
        float dx = event.screenX - lastClickX_;
        float dy = event.screenY - lastClickY_;
        if (std::sqrt(dx * dx + dy * dy) <= dragThreshold_) {
            isDouble = true;
        }
    }

    if (isDouble) {
        lastClickTime_ = -1.0;  // 重置，防止三击误识别
        callback_(Gesture::DoubleClick, event);
    } else {
        lastClickTime_ = event.timestamp;
        lastClickX_ = event.screenX;
        lastClickY_ = event.screenY;
        callback_(Gesture::Click, event);
    }
}

void InputManager::cancelActiveGesture() {
    if (state_ == State::OneFingerDrag) {
        InputEvent event;
        event.type = InputEvent::Type::PointerUp;
        event.screenX = trackLastX_;
        event.screenY = trackLastY_;
        callback_(Gesture::DragEnd, event);
    }
    if (pinchActive_) {
        InputEvent event;
        event.type = InputEvent::Type::PinchEnd;
        callback_(Gesture::PinchEnd, event);
    }
    state_ = State::Idle;
    pinchActive_ = false;
    suppressClick_ = true;
    pinchSession_ = PinchSession{};
}

} // namespace earth_engine
