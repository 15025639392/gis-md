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

// 合成 pinch 的两个虚拟指针相对光标的半间距(物理像素)。任意正值都可以——
// 下游只用质心与我们**直接填好的** pinchScaleFromStart/twist,不再从两指反解;
// 取个不为零的值只是为了让 spread 有定义、且事件形状与真触摸一致。
constexpr float kSyntheticPinchHalfSpreadPixels = 50.0f;

// 鼠标按钮位掩码(与 DOM MouseEvent.buttons 同构)。
constexpr int kButtonPrimary = 1;
constexpr int kButtonSecondary = 2;
constexpr int kButtonMiddle = 4;

bool sameModifiers(const InputEvent::Modifiers& a,
                   const InputEvent::Modifiers& b) {
    return a.shift == b.shift && a.ctrl == b.ctrl && a.alt == b.alt &&
           a.meta == b.meta;
}

} // namespace

std::vector<InputManager::DesktopBinding>
InputManager::defaultDesktopBindings() {
    InputEvent::Modifiers none;
    InputEvent::Modifiers ctrl;
    ctrl.ctrl = true;
    return {
        {DesktopTrigger::LeftDrag, none, DesktopAction::AnchorDrag},
        {DesktopTrigger::Wheel, none, DesktopAction::Zoom},
        {DesktopTrigger::MiddleDrag, none, DesktopAction::Tilt},
        {DesktopTrigger::RightDrag, none, DesktopAction::Zoom},
        {DesktopTrigger::LeftDrag, ctrl, DesktopAction::Tilt},
    };
}

InputManager::DesktopAction InputManager::resolveDesktopAction(
    DesktopTrigger trigger, const InputEvent::Modifiers& modifiers) const {
    for (const DesktopBinding& b : desktopBindings_) {
        if (b.trigger == trigger && sameModifiers(b.modifiers, modifiers)) {
            return b.action;
        }
    }
    return DesktopAction::None;
}

void InputManager::emitSyntheticPinch(const InputEvent& source,
                                      Gesture gesture,
                                      float centroidX,
                                      float centroidY,
                                      float scaleFromStart,
                                      InputEvent::PinchMode mode) {
    InputEvent e = source;
    e.type = gesture == Gesture::PinchStart   ? InputEvent::Type::PinchStart
             : gesture == Gesture::PinchMove  ? InputEvent::Type::PinchMove
                                              : InputEvent::Type::PinchEnd;
    e.pointerCount = 2;
    // 两指对称落在质心两侧 ⇒ 质心恰为目标像素,且 twist 恒为 0。
    e.hasPointerPair = true;
    e.pointer0X = centroidX - kSyntheticPinchHalfSpreadPixels;
    e.pointer0Y = centroidY;
    e.pointer1X = centroidX + kSyntheticPinchHalfSpreadPixels;
    e.pointer1Y = centroidY;
    e.screenX = centroidX;
    e.screenY = centroidY;
    // 直接填绝对派生量:桌面通道**不走 latch 判定**(鼠标的意图由按键/滚轮显式
    // 给出,没有"双指平移与双指俯仰在输入端同形"那个不可消除的歧义)。
    e.pinchScaleFromStart = scaleFromStart;
    e.twistFromStartRadians = 0.0f;
    e.pinchMode = mode;
    callback_(gesture, e);
}

void InputManager::handleWheel(const InputEvent& event) {
    const DesktopAction action =
        resolveDesktopAction(DesktopTrigger::Wheel, event.modifiers);
    if (action != DesktopAction::Zoom) {
        return;
    }
    // **每一格滚轮 = 一次完整的微会话**(Start→Move→End)。这样每格都在光标处
    // 重新取锚点(= 朝光标缩放),且 Start 与 Move 时间戳相同 ⇒ dt=0 ⇒ 不种 zoom
    // 惯性,滚轮给出干脆的离散步进而不是甩飞。
    const float scale =
        static_cast<float>(std::exp(event.wheelDelta * wheelZoomLogStep_));
    emitSyntheticPinch(event, Gesture::PinchStart, event.screenX,
                       event.screenY, 1.0f,
                       InputEvent::PinchMode::Manipulate);
    emitSyntheticPinch(event, Gesture::PinchMove, event.screenX,
                       event.screenY, scale,
                       InputEvent::PinchMode::Manipulate);
    emitSyntheticPinch(event, Gesture::PinchEnd, event.screenX,
                       event.screenY, scale,
                       InputEvent::PinchMode::Manipulate);
}

bool InputManager::processDesktopEvent(const InputEvent& event) {
    if (event.type == InputEvent::Type::Wheel) {
        handleWheel(event);
        return true;
    }
    if (event.pointerType != InputEvent::PointerType::Mouse) {
        return false;
    }

    if (event.type == InputEvent::Type::PointerDown) {
        DesktopTrigger trigger = DesktopTrigger::LeftDrag;
        if (event.buttons & kButtonMiddle) {
            trigger = DesktopTrigger::MiddleDrag;
        } else if (event.buttons & kButtonSecondary) {
            trigger = DesktopTrigger::RightDrag;
        } else if (!(event.buttons & kButtonPrimary)) {
            return false;  // 没有已知按键:交给原路径
        }
        const DesktopAction action =
            resolveDesktopAction(trigger, event.modifiers);
        if (action == DesktopAction::AnchorDrag) {
            return false;  // 走既有的单指锚点拖拽通道
        }
        if (action == DesktopAction::None) {
            return true;   // 显式不响应:消费掉,不要漏到拖拽去
        }
        desktopSessionActive_ = true;
        desktopAction_ = action;
        desktopStartX_ = event.screenX;
        desktopStartY_ = event.screenY;
        suppressClick_ = true;
        emitSyntheticPinch(
            event, Gesture::PinchStart, event.screenX, event.screenY, 1.0f,
            action == DesktopAction::Tilt ? InputEvent::PinchMode::Pitch
                                          : InputEvent::PinchMode::Manipulate);
        return true;
    }

    if (!desktopSessionActive_) {
        return false;
    }

    if (event.type == InputEvent::Type::PointerMove) {
        if (desktopAction_ == DesktopAction::Tilt) {
            // 质心跟随光标 ⇒ 质心 Y 相对基线绝对映射 pitch,与双指 Pitch **逐字
            // 同一段代码**。scale 恒 1(不缩放)。
            emitSyntheticPinch(event, Gesture::PinchMove, event.screenX,
                               event.screenY, 1.0f,
                               InputEvent::PinchMode::Pitch);
        } else {
            // 右键竖拖 zoom:质心**钉在起手处**不跟光标——跟了就会触发 pin 的
            // 横向世界运动(= 拖着地球跑),而右键拖的语义只有缩放。
            const float dy = event.screenY - desktopStartY_;
            const float scale = static_cast<float>(
                std::exp(-dy * dragZoomLogStepPerPixel_));
            emitSyntheticPinch(event, Gesture::PinchMove, desktopStartX_,
                               desktopStartY_, scale,
                               InputEvent::PinchMode::Manipulate);
        }
        return true;
    }

    if (event.type == InputEvent::Type::PointerUp) {
        emitSyntheticPinch(event, Gesture::PinchEnd, desktopStartX_,
                           desktopStartY_, 1.0f,
                           desktopAction_ == DesktopAction::Tilt
                               ? InputEvent::PinchMode::Pitch
                               : InputEvent::PinchMode::Manipulate);
        desktopSessionActive_ = false;
        desktopAction_ = DesktopAction::None;
        return true;
    }
    return false;
}

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
        desktopSessionActive_ = false;
        desktopAction_ = DesktopAction::None;
        cancelActiveGesture();
        return;
    }

    // 桌面绑定优先:命中就整条消费,不再漏到触摸路径(否则中键拖会同时产出
    // tilt 和 anchor drag)。
    if (processDesktopEvent(event)) {
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
    desktopSessionActive_ = false;
    desktopAction_ = DesktopAction::None;
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
