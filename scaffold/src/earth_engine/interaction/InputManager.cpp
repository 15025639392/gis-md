#include "InputManager.h"
#include <cmath>

namespace earth_engine {

void InputManager::process(const InputEvent& event) {
    if (!callback_) return;

    if (event.type == InputEvent::Type::Cancel) {
        cancelActiveGesture();
        return;
    }

    // ---- Pinch 事件直通 ----
    if (event.isPinchEvent()) {
        switch (event.type) {
            case InputEvent::Type::PinchStart:
                cancelActiveGesture();
                state_ = State::TwoFinger;
                pinchActive_ = true;
                suppressClick_ = true;
                callback_(Gesture::PinchStart, event);
                break;
            case InputEvent::Type::PinchMove:
                if (!pinchActive_) {
                    state_ = State::TwoFinger;
                    pinchActive_ = true;
                    suppressClick_ = true;
                    InputEvent start = event;
                    start.type = InputEvent::Type::PinchStart;
                    start.pinchScale = 1.0f;
                    start.rotationRadians = 0.0f;
                    start.centerDeltaX = 0.0f;
                    start.centerDeltaY = 0.0f;
                    callback_(Gesture::PinchStart, start);
                }
                callback_(Gesture::PinchMove, event);
                break;
            case InputEvent::Type::PinchEnd:
                if (pinchActive_) {
                    callback_(Gesture::PinchEnd, event);
                }
                state_ = State::Idle;
                pinchActive_ = false;
                suppressClick_ = true;
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
}

} // namespace earth_engine
