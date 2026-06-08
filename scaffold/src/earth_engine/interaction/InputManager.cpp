#include "InputManager.h"
#include <cmath>

namespace earth_engine {

void InputManager::process(const InputEvent& event) {
    if (!callback_) return;

    // ---- Pinch 事件直通 ----
    if (event.isPinchEvent()) {
        switch (event.type) {
            case InputEvent::Type::PinchStart:
                tracking_ = false;
                dragging_ = false;
                callback_(Gesture::PinchStart, event);
                break;
            case InputEvent::Type::PinchMove:
                callback_(Gesture::PinchMove, event);
                break;
            case InputEvent::Type::PinchEnd:
                callback_(Gesture::PinchEnd, event);
                tracking_ = false;
                dragging_ = false;
                break;
            default:
                break;
        }
        return;
    }

    // ---- 指针事件 ----
    switch (event.type) {
        case InputEvent::Type::PointerDown:
            tracking_ = true;
            dragging_ = false;
            trackStartX_ = event.screenX;
            trackStartY_ = event.screenY;
            trackLastX_ = event.screenX;
            trackLastY_ = event.screenY;
            break;

        case InputEvent::Type::PointerMove:
            if (!tracking_) break;

            if (!dragging_) {
                float dx = event.screenX - trackStartX_;
                float dy = event.screenY - trackStartY_;
                if (std::sqrt(dx * dx + dy * dy) >= dragThreshold_) {
                    dragging_ = true;
                    // DragStart 使用起始位置
                    InputEvent dragStartEvent = event;
                    dragStartEvent.type = InputEvent::Type::PointerDown;
                    dragStartEvent.screenX = trackStartX_;
                    dragStartEvent.screenY = trackStartY_;
                    callback_(Gesture::DragStart, dragStartEvent);
                }
            }

            if (dragging_) {
                callback_(Gesture::DragMove, event);
                trackLastX_ = event.screenX;
                trackLastY_ = event.screenY;
            }
            break;

        case InputEvent::Type::PointerUp: {
            if (!tracking_) break;
            tracking_ = false;

            if (dragging_) {
                dragging_ = false;
                callback_(Gesture::DragEnd, event);
            } else {
                // 无拖拽 → click
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
            break;
        }

        default:
            break;
    }
}

void InputManager::reset() {
    tracking_ = false;
    dragging_ = false;
    lastClickTime_ = -1.0;
}

} // namespace earth_engine
