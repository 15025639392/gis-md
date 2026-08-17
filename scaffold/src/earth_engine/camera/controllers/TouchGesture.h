#pragma once

#include <cstdint>

namespace earth_engine {

/// 双指手势模式（与 InputEvent::PinchMode 同构；相机层不依赖 interaction 头，
/// 由 SceneInputCoordinator 显式映射）。
enum class PinchMode : uint8_t {
    Undecided,   ///< latch 窗口内：施加 zoom/twist，锚点钉起手质心
    Manipulate,  ///< zoom+twist+刚性 pan（锚点钉当前质心）
    Pitch        ///< 双指平行竖移倾斜（锚点钉 latch 质心，质心Y驱动 pitch）
};

/// 双指手势输入（绝对量表述：事件被合并/丢弃不产生累积漂移）。
struct PinchInput {
    float scaleFromStart = 1.0f;      ///< 当前 spread / 起手 spread
    float twistFromStartRadians = 0.0f;  ///< 连线角累计（unwrap）
    float centroidX = 0.0f;           ///< 双指质心（物理像素）
    float centroidY = 0.0f;
    PinchMode mode = PinchMode::Manipulate;
    /// 每轴激活（契约 2.2）：缩放/旋转超过阈值后保持；默认 true 兼容旧路径。
    bool zoomEngaged = true;
    bool rotateEngaged = true;
    /// 滚轮合成捏合的平滑缩放（契约 3.1）：~300ms 指数收敛，不瞬时跳变。
    bool smoothZoom = false;
    double timestamp = 0.0;
};

/// 吃触摸手势的控制器所实现的接口。
///
/// **刻意与 `ICameraController` 分开**:后者是"每帧驱动相机"的契约,前者是"吃这一
/// 种输入"的能力。`FlightController` 只实现前者(飞行不吃输入),`FreeGlobeController`
/// 与 `TetheredController` 两者都实现。硬把输入塞进 `ICameraController` 就会逼出
/// 一堆空实现,而且下一个吃滚轮不吃触摸的桌面控制器又得再塞一批。
///
/// 编排层靠 `CameraControllerSelector::activeAs<ITouchGestureTarget>()` 路由——
/// 返回 nullptr = 当前驱动者不吃触摸(如飞行中),事件丢弃。
///
/// ⚠️ 语义**因控制器而异,这是设计不是缺陷**:Free 下拖拽是"抓住地表点跟手"
/// (绕地心的锚点钉合),Tethered 下是"绕载体转"(载体在屏幕中心不动)。同一个事件
/// 两套数学,正是它们必须是两个类而不是一个带 mode 分支的类的原因。
class ITouchGestureTarget {
public:
    virtual ~ITouchGestureTarget() = default;

    virtual void onDragStart(float xPixels, float yPixels, double timestamp) = 0;
    virtual void onDragMove(float xPixels, float yPixels, double timestamp) = 0;
    virtual void onDragEnd() = 0;
    /// drag 被系统取消（来电、系统手势、新手势抢占）：立即停、无惯性。
    /// 默认空实现；有惯性的控制器覆盖为"清惯性 + 作废锚点"。
    virtual void onDragCancel() {}

    virtual void onPinchGesture(const PinchInput& input) = 0;
    virtual void onPinchEnd() = 0;
    /// pinch 被系统取消：立即停、不启动任何惯性（含 zoom 惯性）。
    virtual void onPinchCancel() {}

    /// 是否有活动的双指手势。编排层据此判「手势起手帧」——起手前要先跑一个
    /// 同步帧,让手势数学从一个约束已满足的位姿起步。
    virtual bool pinching() const = 0;
};

} // namespace earth_engine
