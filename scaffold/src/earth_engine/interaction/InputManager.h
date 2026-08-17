#pragma once

#include "InputEvent.h"
#include <functional>
#include <cstdint>
#include <vector>

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
        DragStart,    // → CameraSystem::onDragStart
        DragMove,     // → CameraSystem::onDragMove
        DragEnd,      // → CameraSystem::onDragEnd（可能启动惯性）
        DragCancel,   // → CameraSystem::onDragCancel（系统取消：立即停、无惯性）
        PinchStart,   // → CameraSystem::onPinch
        PinchMove,    // → CameraSystem::onPinch
        PinchEnd,     // → 重置 pinch 状态
        PinchCancel,  // → CameraSystem::onPinchCancel（系统取消：不启动惯性）
        Click,        // → pick + onSelect
        DoubleClick,  // → zoom-to 或 pick + onSelect
        KeyCommand,   // → CameraSystem::onKeyCommand（契约 3.3 键盘）
    };

    /// 回调：Scene 注册此回调来处理识别出的手势
    /// @param gesture  识别出的手势
    /// @param event    触发手势的事件（包含 screenX/Y、timestamp、modifiers）
    using Callback = std::function<void(Gesture gesture, const InputEvent& event)>;

    // ---- 桌面输入绑定表(Cesium 式 EventType × Modifiers)----
    //
    // ⚠️⚠️ **桌面绑定是翻译层,不是第二套相机数学**。滚轮/中键/右键全部被合成成
    // 与双指完全相同的 `PinchStart/Move/End` 事件(`hasPointerPair=true`,两个虚拟
    // 指针对称落在光标两侧 ⇒ 质心即光标),于是"滚轮 zoom 与双指 zoom 走同一锚点
    // 通道""中键 tilt 与双指 Pitch 同语义"这两条判据是**构造上成立**的,而不是靠
    // 两份实现碰巧一致。相机层完全不知道有鼠标这回事。

    enum class DesktopTrigger : uint8_t {
        LeftDrag,
        MiddleDrag,
        RightDrag,
        Wheel
    };

    enum class DesktopAction : uint8_t {
        /// 走单指锚点拖拽通道(抓住地表点跟手)。
        AnchorDrag,
        /// 合成 `PinchMode::Manipulate`,只 dolly 不 pan(质心钉在起手处)。
        Zoom,
        /// 合成 `PinchMode::Pitch`,质心 Y 驱动俯仰 —— 与双指 Pitch 逐字同语义。
        Tilt,
        /// 不响应。
        None
    };

    struct DesktopBinding {
        DesktopTrigger trigger = DesktopTrigger::LeftDrag;
        /// **精确匹配**(与 Cesium 一致:一个 binding 要么不带修饰键,要么带一个)。
        /// 不做"包含"匹配 —— 那样 ctrl+shift+左键会同时命中 ctrl 和无修饰两条。
        InputEvent::Modifiers modifiers;
        DesktopAction action = DesktopAction::AnchorDrag;
    };

    /// 默认表(对齐 Cesium `ScreenSpaceCameraController` 的桌面习惯):
    ///   左键拖 → 锚点拖拽 / 滚轮 → zoom / 中键拖 → tilt
    ///   右键拖 → zoom / Ctrl+左键拖 → tilt
    /// ⚠️ Cesium 还有 Shift+左键 = look(原地转视线)。**本表刻意不含它**:那需要
    /// 一个"绕相机自身转"的原语,`setViewpoint({heading,pitch})` 已经能表达,但把它
    /// 接到拖拽手势上是独立的一件事,不该顺手塞进绑定表凑数。
    static std::vector<DesktopBinding> defaultDesktopBindings();

    void setDesktopBindings(std::vector<DesktopBinding> bindings) {
        desktopBindings_ = std::move(bindings);
    }
    const std::vector<DesktopBinding>& desktopBindings() const {
        return desktopBindings_;
    }
    /// 查表。无匹配 ⇒ `None`。
    DesktopAction resolveDesktopAction(
        DesktopTrigger trigger, const InputEvent::Modifiers& modifiers) const;

    /// 滚轮一格对应的对数缩放步长。正 delta = 拉近。
    void setWheelZoomLogStep(double step) { wheelZoomLogStep_ = step; }
    /// 右键竖拖每像素的对数缩放步长。
    void setDragZoomLogStepPerPixel(double step) {
        dragZoomLogStepPerPixel_ = step;
    }

    InputManager() : desktopBindings_(defaultDesktopBindings()) {}

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
    /// 桌面合成手势:返回 true 表示本事件已被桌面通道消费,不再走触摸路径。
    bool processDesktopEvent(const InputEvent& event);
    void handleWheel(const InputEvent& event);
    /// 发一条合成 pinch 事件(两个虚拟指针对称落在 centroid 两侧)。
    void emitSyntheticPinch(const InputEvent& source,
                            Gesture gesture,
                            float centroidX,
                            float centroidY,
                            float scaleFromStart,
                            InputEvent::PinchMode mode,
                            bool smoothZoom = false);
    void cancelActiveGesture();
    /// 双指会话：从 pointer0/1 计算派生量（spread 比、twist unwrap 累计），
    /// 并按契约 2.2 做**每轴独立激活**（缩放/旋转超阈值后保持；倾斜由起手
    /// 竖直锁单独决定；平移随动）。event 为可写副本，填充后回调转发。
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
        // 每轴激活（一旦 engage 整段手势保持）：
        bool zoomEngaged = false;
        bool rotateEngaged = false;
        bool panEngaged = false;
        // 倾斜起手竖直锁：一旦判定（latch 或 reject）不再改判。
        bool pitchLatched = false;
        bool pitchRejected = false;
        double firstMoveTime = 0.0;  // 一指先动时等待第二指的时间窗起点
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

    // 桌面合成会话(中键/右键/Ctrl+左键拖)
    bool desktopSessionActive_ = false;
    DesktopAction desktopAction_ = DesktopAction::None;
    float desktopStartX_ = 0.0f;
    float desktopStartY_ = 0.0f;

    std::vector<DesktopBinding> desktopBindings_;
    double wheelZoomLogStep_ = 0.20;
    double dragZoomLogStepPerPixel_ = 0.005;

    // 配置
    float dragThreshold_ = 8.0f;            // 像素
    double doubleClickInterval_ = 0.35;     // 秒
};

} // namespace earth_engine
