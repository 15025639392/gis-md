#pragma once

#include "InputEvent.h"
#include <functional>
#include <cstdint>

namespace earth_engine {

/// 手势识别器。
///
/// 接收归一化 InputEvent 流，识别 drag、pinch、click、double-click。
/// 不直接操作 Camera 或 Selection — 通过回调通知拥有者（Scene）。
///
/// 约定：
///   - 平台仍负责"区分单指/双指"（iOS: UIPanGestureRecognizer vs UIPinchGestureRecognizer；
///     Android: MotionEvent.getPointerCount()）。Pinch 事件以 PinchStart/PinchMove/PinchEnd
///     类型直接传入。
///   - 单指 PointerDown/PointerMove/PointerUp 由 InputManager 识别 click、double-click 和 drag。
class InputManager {
public:
    /// 手势回调类型
    enum class Gesture {
        DragStart,    // → CameraController::onDragStart
        DragMove,     // → CameraController::onDragMove
        DragEnd,      // → CameraController::onDragEnd（可能启动惯性）
        PinchStart,   // → CameraController::onPinch
        PinchMove,    // → CameraController::onPinch
        PinchEnd,     // → 重置 pinch 状态
        Click,        // → pick + onSelect
        DoubleClick,  // → zoom-to 或 pick + onSelect
    };

    /// 回调：Scene 注册此回调来处理识别出的手势
    /// @param gesture  识别出的手势
    /// @param event    触发手势的事件（包含 screenX/Y、timestamp、modifiers）
    using Callback = std::function<void(Gesture gesture, const InputEvent& event)>;

    InputManager() = default;

    /// 设置手势回调
    void setCallback(Callback cb) { callback_ = std::move(cb); }

    /// 处理一个输入事件
    void process(const InputEvent& event);

    /// 重置状态（手势中断、场景销毁时调用）
    void reset();

    // ---- 配置 ----

    /// 拖拽阈值（像素），超过此位移后识别为 drag 而非 click
    void setDragThreshold(float pixels) { dragThreshold_ = pixels; }

    /// 双击时间窗口（秒）
    void setDoubleClickInterval(double seconds) { doubleClickInterval_ = seconds; }

private:
    enum class State {
        Idle,
        OneFingerPending,
        OneFingerDrag,
        TwoFinger
    };

    void finishPointerGesture(const InputEvent& event);
    void cancelActiveGesture();
    /// 双指会话：从 pointer0/1 计算派生量（spread 比、twist unwrap 累计），
    /// 并在起手窗口内做一次 mode latch。event 为可写副本，填充后回调转发。
    void processPinchWithPointerPair(InputEvent& event);

    /// 双指会话状态（首个携带 pointer pair 的事件初始化，PinchEnd/Cancel 清空）
    struct PinchSession {
        bool baselineValid = false;
        float spread0 = 1.0f;
        float angle0 = 0.0f;         // 起手两指连线角
        float centroid0X = 0.0f;
        float centroid0Y = 0.0f;
        float p0StartX = 0.0f, p0StartY = 0.0f;
        float p1StartX = 0.0f, p1StartY = 0.0f;
        double t0 = 0.0;
        float prevAngleRaw = 0.0f;   // unwrap 用
        float angleUnwrapped = 0.0f; // 累计连线角（连续，无 ±π 跳变）
        InputEvent::PinchMode mode = InputEvent::PinchMode::Undecided;
    };
    PinchSession pinchSession_;

    Callback callback_;

    State state_ = State::Idle;

    // 拖拽状态
    float trackStartX_ = 0.0f;
    float trackStartY_ = 0.0f;
    float trackLastX_ = 0.0f;
    float trackLastY_ = 0.0f;
    bool suppressClick_ = false;
    bool pinchActive_ = false;

    // 双击检测
    double lastClickTime_ = -1.0;
    float lastClickX_ = 0.0f;
    float lastClickY_ = 0.0f;

    // 配置
    float dragThreshold_ = 8.0f;            // 像素
    double doubleClickInterval_ = 0.35;     // 秒
};

} // namespace earth_engine
