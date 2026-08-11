#pragma once

#include "../core/math/Vec3.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <functional>

namespace earth_engine {

class Camera;
class Ray;
class CameraConstraintSolver;

/// 球面（Free 模式）手势操控器：把触摸事件翻译成相机位姿变化。
///
/// 分层位置：**输入 → 位姿** 这一层。它不拥有相机，也不拥有约束求解器，
/// 只持有二者的指针；每次改完位姿立刻走 `clampNow` 过一遍约束出口。
/// 编排层（CameraController）负责帧循环、帧末哨兵、视点设定与只读派生量。
///
/// 为什么独立成类：后面还会有语义完全不同的第二、第三个实现——Tethered
/// 模式（拖拽=绕载体转，不是绕地心转）与桌面输入（滚轮/中键/方向键，事件
/// 形状不同但共用锚点数学）。焊在一个类里就只能靠 `if (mode == ...)` 把两套
/// 数学缝进同一个函数体。
///
/// 核心不变量（与 CameraController 时代逐字保留）：
/// - 单指拖拽先抓取地表点，移动时让该点尽量跟随手指；
/// - 双指手势中**只有锚点钉合(pin)产生横向世界运动**，dolly/twist/pitch 在
///   数学上全部严格保锚，与 pan 正交——没有意图分类。
class GlobeGestureManipulator {
public:
    /// @param camera 受控相机（非空，生命周期由调用者管理）
    /// @param solver 约束求解器（非空，由编排层拥有）
    GlobeGestureManipulator(Camera* camera, CameraConstraintSolver* solver);

    /// 设置视口尺寸（用于 pick ray 和屏幕坐标归一化）
    void setViewport(int widthPixels, int heightPixels);

    /// Scene 可注入地形拾取链路；未注入或未命中时回退到内部的 WGS84 球面拾取。
    using SurfacePicker = std::function<bool(float xPixels, float yPixels, Vec3& outPoint)>;
    void setSurfacePicker(SurfacePicker picker);

    /// drag 开始（手指按下）
    /// @param timestamp 单调时钟时间戳（秒），用于惯性角速度计算
    void onDragStart(float xPixels, float yPixels, double timestamp = 0.0);

    /// drag 移动（手指滑动）
    void onDragMove(float xPixels, float yPixels, double timestamp = 0.0);

    /// drag 结束（手指抬起，启动惯性）
    void onDragEnd();

    /// 双指手势模式（与 InputEvent::PinchMode 同构；相机层不依赖 interaction
    /// 头，由 SceneInputCoordinator 显式映射）。
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
        double timestamp = 0.0;
    };
    void onPinchGesture(const PinchInput& input);

    /// 旧契约薄适配器（音量键合成捏合 / 无 pointer pair 的平台）：把每事件
    /// 增量累积成绝对量后转发新接口，mode 恒 Manipulate。centerDeltaX/Y 不再
    /// 消费（倾斜走 Pitch 模式，由 InputManager latch 判定）。迁移完成后删除。
    void onPinchGesture(float scale,
                        float centerX,
                        float centerY,
                        float rotationRadians,
                        float centerDeltaX,
                        float centerDeltaY,
                        double timestamp = 0.0);
    void onPinchEnd();

    /// 是否有活动的双指手势。编排层据此判定"手势起手帧"（起手前要先跑一个
    /// 同步帧，见 CameraController 的转发层）。
    bool pinching() const { return pinching_; }

    /// 惯性/滑行/回中的时间步进。编排层在冻结与脚本平移的早退**之后**调用，
    /// 故本函数不认识这两种测量台状态。
    void tick(double deltaSeconds);

    /// pan 惯性的"已经停了"阈值(rad/s)。低于它不再产生任何位移,故等价于零。
    /// **三处必须同用这一个常量**:运动闸、自清零、isAnimating 判据。
    /// 各写各的字面量正是这类 bug 的温床 —— 判据比消费者宽一点点,表现就是
    /// 「拖一下之后永远不再空闲」,而画面上完全看不出区别。
    static constexpr double kMinInertiaAngularVelocity = 0.0001;

    /// 是否仍在自行滑行（惯性/zoom 滑行）。**不含**编排层的脚本平移。
    bool isAnimating() const {
        return hasZoomInertia_ ||
               inertiaAngularVelocity_ > kMinInertiaAngularVelocity;
    }

    // ---- 惯性清零：三个**不同**的子集 ----
    //
    // ⚠️ 这三档是从拆分前逐字保留的现状，不是设计出来的层级：
    // viewDistance/setNadirOrbitView 只清 pan、resetNorthUp 连 zoom 一起清、
    // 冻结/脚本平移再加上回中欠账。差异是否有意暂无定论——统一它属于行为
    // 变更，不该混进"纯搬运"这一刀里。要清理请单开一次。
    void clearPanInertia();
    /// = clearPanInertia + zoom 滑行
    void clearGlideInertia();
    /// = clearGlideInertia + 高空回中欠账
    void clearAllInertia();

    /// 当前手势锚点世界坐标(ECEF)。有活动 drag/pinch 锚点时返回 true 并
    /// 写出 outWorld；否则返回 false。测试用查询（锚点获取/重试行为断言）。
    bool debugAnchorWorld(Vec3& outWorld) const;

private:
    /// 锚点钉合求解：求把"像素 (x,y) 射线与抓取球的交点方向"转到 anchorNormal
    /// 的绕地心旋转。单指拖拽与双指钉合共用同一份数学（同构，见 applyAnchorDrag
    /// 与 applyPinchPin），任何一侧的修正必须同时作用于另一侧。
    struct AnchorSolveResult {
        glm::dquat delta{1.0, 0.0, 0.0, 0.0};
        /// 入射余弦 |dot(rayDir, 求解点法线)|：1=正对，→0=掠射；最近接近点
        /// 恒为 0（射线在该点与球面相切向正交）。病态区权重的输入。
        double conditioning = 0.0;
        bool valid = false;       ///< 求出了球面点（真命中或最近接近点）
        bool hit = false;         ///< 射线真正命中抓取球
        bool degenerate = false;  ///< from≈to，无需旋转
    };
    AnchorSolveResult solveAnchorRotation(const Vec3& anchorNormal,
                                          float xPixels,
                                          float yPixels) const;
    /// 抓取球上"该射线之下"的点：真交点，或 miss 时的最近接近点（相切处
    /// 与真交点重合 → 跨球缘连续）。球心在射线后方等极端退化时返回 false。
    bool pointOnGrabSphere(const Ray& ray, Vec3& outPoint, bool& outTrueHit) const;
    /// 转台式旋转增量：屏幕像素位移按 fov/height 转角度。
    glm::dquat turntableDeltaFromPixels(double dx, double dy) const;
    /// 单指转台（相对 dragLast）。
    glm::dquat spinTurntableDelta(float xPixels, float yPixels) const;

    bool intersectGrabSphere(const Ray& ray, Vec3& outPoint) const;
    static bool intersectSphere(const Ray& ray, double radiusMeters,
                                Vec3& outPoint);
    bool pickSurfacePoint(float xPixels, float yPixels, Vec3& outPoint) const;
    bool grabSurfacePoint(float xPixels, float yPixels);
    /// pinch 锚点获取：pick 地表点 → 半径钳到 eye 以下（防抓取球包住相机、
    /// 射线命中球背面疯转）→ 方向换成射线∩钳位球（防起手跳变）。
    bool tryAcquirePinchAnchor(float xPixels, float yPixels);
    /// pinch 钉合：把锚点钉到目标像素，病态区连续混入质心转台并整点重取
    /// 锚点（与单指 applyAnchorDrag 同一套连续化策略），末尾做高度钳位。
    /// @return 实际施加的旋转增量（未施加时为单位四元数），供 pan 惯性累积。
    glm::dquat applyPinchPin(float targetX, float targetY);
    void applyAnchorDrag(float xPixels, float yPixels, double timestamp);
    /// @return 是否实际施加（被守卫拒绝时返回 false，供 Pitch 反 wind-up）
    bool rotateCameraVerticalAroundPoint(const glm::dvec3& center,
                                         double angle,
                                         double minSlope);
    /// 高空 zoom-out 回中（预算松弛，见 .cpp 常量说明）：手势/滑行期只充值
    /// 预算（不动相机，与锚点钉合严格正交），松手后 tick() 指数消费。
    void accrueRecenterBudget(double zoomOutLogStep);
    void consumeRecenterBudget(double deltaSeconds);

    /// 手势/惯性路径的位姿钳位：调用方刚显式动过相机，故恒 user-driven、
    /// 无帧间隔（dt=0）。与编排层的帧末哨兵是**两条性质不同的路径**——前者是
    /// "我刚动了，钳一下"，后者是"检查有没有人绕过我"——别再合并回一个带
    /// source 枚举的函数：那样每个调用点都要现场装配一个 context，而枚举唯一
    /// 的作用只是区分是不是帧末。
    /// @param pinnedAnchorWorld 非空 → 碰撞解算沿 eye→anchor 直线退出（严格
    ///        保锚；径向抬升会泄漏 anchorErr）。指针仅在调用栈内有效。
    /// @return 位姿是否被修改
    bool clampNow(const glm::dvec3* pinnedAnchorWorld);

    Camera* camera_;
    CameraConstraintSolver* solver_;
    SurfacePicker surfacePicker_;
    int viewportWidth_ = 1;
    int viewportHeight_ = 1;

    // drag 状态
    bool dragging_ = false;
    float dragStartX_ = 0.0f;
    float dragStartY_ = 0.0f;
    float dragLastX_ = 0.0f;
    float dragLastY_ = 0.0f;
    bool hasGrabbedPoint_ = false;
    Vec3 grabbedNormal_{0.0, 0.0, 1.0};
    Vec3 grabbedPoint_{0.0, 0.0, 6378137.0};
    double grabbedRadiusMeters_ = 6378137.0;

    // 惯性状态
    glm::dvec3 inertiaAxis_{0.0, 1.0, 0.0};
    double inertiaAngularVelocity_ = 0.0;
    double lastDragTimestamp_ = 0.0;  // 最近一次 drag 事件的时间戳

    // pinch 状态
    bool pinching_ = false;
    bool hasPinchAnchor_ = false;
    Vec3 pinchAnchorNormal_{0.0, 0.0, 1.0};
    float pinchAnchorScreenX_ = 0.0f;   // 起手质心（Undecided 期 pin 目标）
    float pinchAnchorScreenY_ = 0.0f;
    double lastPinchTimestamp_ = 0.0;
    PinchMode pinchActiveMode_ = PinchMode::Undecided;
    // 已施加的累计缩放/拧动（绝对量状态；与输入的差 = 本事件增量）。
    // jerk 限幅 = 单事件增量夹到 ±ln(1.3)，落后量夹到 ±kMaxPinchScaleResidualLog
    // （防长时间饱和攒出松手仍在补的欠账），语义等价旧残差机制。
    double pinchAppliedScaleLog_ = 0.0;
    double pinchAppliedTwistRadians_ = 0.0;
    // Pitch 模式：质心 Y 相对基线绝对映射 pitch；被守卫拒绝时重取基线
    // （零死区离合，防 wind-up），锚点钉 latch 时刻的质心像素。
    float pitchBaselineY_ = 0.0f;
    double pitchAppliedRadians_ = 0.0;
    float pitchPinX_ = 0.0f;
    float pitchPinY_ = 0.0f;
    // 上一事件质心（pin 病态区转台回退的位移基准）。
    float lastPinchCentroidX_ = 0.0f;
    float lastPinchCentroidY_ = 0.0f;
    // 旧契约适配器的每事件增量累计（新契约不使用）。
    double adapterScaleLog_ = 0.0;
    double adapterTwistRadians_ = 0.0;
    // 双指 pan 惯性累积（EMA，静止帧自然衰减向 0）：松手时种进与单指拖拽
    // 共用的 inertiaAxis_/inertiaAngularVelocity_ 通道。zoomInertiaAnchor_
    // 是固定世界点，pan 惯性转的是相机（rotateAboutOrigin），世界点不动，
    // 双惯性并行无需同转锚点——dolly 朝固定世界点在任意相机旋转下都正确。
    glm::dvec3 pinchPanAxis_{0.0, 1.0, 0.0};
    double pinchPanAngularVelocity_ = 0.0;

    // 高空回中欠账（弧度）。手势/滑行期充值，无手势时 tick() 消费。
    double recenterBudgetRadians_ = 0.0;

    // zoom 惯性状态（对数距离空间，见 .cpp 常量说明）
    bool hasZoomInertia_ = false;
    double zoomInertiaLogRate_ = 0.0;         // d(ln dist)/dt，>0 = 继续拉近
    glm::dvec3 zoomInertiaAnchor_{0.0, 0.0, 0.0};
};

} // namespace earth_engine
