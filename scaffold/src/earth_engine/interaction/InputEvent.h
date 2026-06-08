#pragma once

#include <cstdint>

namespace earth_engine {

/// 归一化输入事件。
///
/// 所有平台输入（iOS UITouch/UIEvent、Android MotionEvent、鼠标）
/// 必须通过平台适配层转换为 InputEvent 后再传给 Engine。
///
/// 坐标约定：
///   - screenX/screenY 单位为物理渲染 surface 像素（已乘以 devicePixelRatio）
///   - 原点在左上角
///   - devicePixelRatio 保留作为 PickingService 的参考
struct InputEvent {

    enum class Type : uint8_t {
        PointerDown,
        PointerMove,
        PointerUp,
        PinchStart,
        PinchMove,
        PinchEnd,
        Key
    };

    enum class PointerType : uint8_t {
        Touch,
        Mouse,
        Pen
    };

    struct Modifiers {
        bool shift  = false;
        bool ctrl   = false;
        bool alt    = false;
        bool meta   = false;

        bool any() const { return shift || ctrl || alt || meta; }
    };

    Type type = Type::PointerDown;

    /// 物理像素坐标（已乘以 devicePixelRatio / contentScaleFactor）
    float screenX = 0.0f;
    float screenY = 0.0f;

    /// 屏幕密度（供 PickingService/unproject 参考）
    float devicePixelRatio = 1.0f;

    PointerType pointerType = PointerType::Touch;

    /// 按钮位掩码（0=none, 1=primary, 2=secondary, …）
    int buttons = 0;

    Modifiers modifiers;

    /// 单调递增时间戳（秒）。来源应为平台单调时钟（如 CACurrentMediaTime、
    /// SystemClock.uptimeMillis），不是挂钟时间。
    double timestamp = 0.0;

    /// PinchMove 时相对上一帧的缩放因子（OpenGlobus TouchNavigation:
    /// zoomCur.length / zoomPrev.length，1.0 = 无缩放）
    float pinchScale = 1.0f;

    /// 双指相对上一帧的旋转角（radian，屏幕坐标系）
    float rotationRadians = 0.0f;

    /// 双指中心相对上一帧的位移（物理像素，screen y 向下）
    float centerDeltaX = 0.0f;
    float centerDeltaY = 0.0f;

    /// 便捷查询
    bool isPointerEvent() const {
        return type == Type::PointerDown ||
               type == Type::PointerMove ||
               type == Type::PointerUp;
    }

    bool isPinchEvent() const {
        return type == Type::PinchStart ||
               type == Type::PinchMove ||
               type == Type::PinchEnd;
    }
};

} // namespace earth_engine
