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
        Cancel,
        Key,
        /// 鼠标滚轮/触控板捏合滚动。`wheelDelta` 带步数。
        Wheel
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

    /// 按钮位掩码。与 DOM `MouseEvent.buttons` 同构:
    ///   1 = 主键(左) / 2 = 次键(右) / 4 = 中键
    /// 桌面绑定表按它区分 Left/Middle/RightDrag。
    int buttons = 0;

    /// 当前有效指针数量。鼠标为 1；双指手势为 2。
    int pointerCount = 1;

    Modifiers modifiers;

    /// 单调递增时间戳（秒）。来源应为平台单调时钟（如 CACurrentMediaTime、
    /// SystemClock.uptimeMillis），不是挂钟时间。
    double timestamp = 0.0;

    /// 滚轮步数(正 = 向前滚 = 拉近)。一"格"约 1.0;触控板惯性滚动可以是小数。
    /// 平台层负责把各自的原始单位(WheelEvent.deltaY / NSEvent.scrollingDeltaY)
    /// 归一到这个尺度 —— 核心层不认识平台单位。
    float wheelDelta = 0.0f;

    /// 键盘键（平台层归一化；契约 3.3 Mapbox 键位）。
    enum class Key : uint8_t {
        None,
        ArrowLeft,
        ArrowRight,
        ArrowUp,
        ArrowDown,
        Plus,    // = / + / 小键盘 +
        Minus    // - / _
    };
    Key key = Key::None;

    /// PinchMove 时相对上一帧的缩放因子（当前双指距离 / 上一帧双指距离，
    /// 1.0 = 无缩放）
    float pinchScale = 1.0f;

    /// 双指相对上一帧的旋转角（radian，屏幕坐标系）
    float rotationRadians = 0.0f;

    /// 双指中心相对上一帧的位移（物理像素，screen y 向下）
    float centerDeltaX = 0.0f;
    float centerDeltaY = 0.0f;

    /// 双指原始位置（物理像素，左上原点）。平台能提供时应填写，
    /// 便于核心层统一判断缩放、旋转、平移、倾斜意图。
    bool hasPointerPair = false;
    float pointer0X = 0.0f;
    float pointer0Y = 0.0f;
    float pointer1X = 0.0f;
    float pointer1Y = 0.0f;

    /// 双指手势模式：InputManager 在起手窗口内做一次判定后整段 latch，
    /// 消除"每事件重分类→阈值附近模式翻转"。双指平移与双指俯仰在输入端
    /// 严格同形（同向平移、间距/连线角不变），分类不可消除，只能一次定死。
    /// 平台无法提供两指原始坐标时恒为 Manipulate（安全默认，等价旧行为）。
    enum class PinchMode : uint8_t {
        Undecided,   ///< latch 窗口内：施加 zoom/twist，锚点钉起手质心
        Manipulate,  ///< zoom+twist+刚性 pan（锚点钉当前质心）
        Pitch        ///< 双指平行竖移倾斜（锚点钉 latch 质心）
    };
    PinchMode pinchMode = PinchMode::Manipulate;

    /// 双指派生量（InputManager 从 pointer0/1 计算，绝对量表述——事件被
    /// 合并/丢弃时不累积漂移）：
    /// 当前 spread / 起手 spread。
    float pinchScaleFromStart = 1.0f;
    /// 两指连线角相对起手的累计旋转（radian，跨帧 unwrap 防 ±π 跳变）。
    float twistFromStartRadians = 0.0f;
    /// 每轴独立激活（契约 2.2，Mapbox 阈值）：缩放/旋转超过阈值后保持激活，
    /// 倾斜由起手竖直锁单独决定（pinchMode==Pitch）；平移随动。平台无法提供
    /// 两指原始坐标时恒为 true（安全默认，等价旧行为）。
    bool pinchZoomEngaged = true;
    bool pinchRotateEngaged = true;
    /// 滚轮合成捏合的平滑缩放标志（契约 3.1）：控制器对目标做短指数收敛
    /// （~300ms），不瞬时跳变、不种 zoom 惯性。
    bool pinchSmoothZoom = false;

    /// 便捷查询
    bool isPointerEvent() const {
        return type == Type::PointerDown ||
               type == Type::PointerMove ||
               type == Type::PointerUp;
    }

    bool isWheelEvent() const { return type == Type::Wheel; }

    bool isPinchEvent() const {
        return type == Type::PinchStart ||
               type == Type::PinchMove ||
               type == Type::PinchEnd;
    }
};

} // namespace earth_engine
