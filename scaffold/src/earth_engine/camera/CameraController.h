#pragma once

#include "../core/math/Vec3.h"
#include "CameraConstraintSolver.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace earth_engine {

class Camera;
class Ray;

/// Anchor-based globe camera controller.
/// 单指拖拽先抓取地表点，移动时让该点尽量跟随手指。
/// 双指手势围绕双指中心下方的地表锚点缩放、旋转和倾斜。
class CameraController {
public:
    /// @param camera 受控相机（非空，生命周期由调用者管理）
    explicit CameraController(Camera* camera);

    /// 设置视口尺寸（用于 pick ray 和屏幕坐标归一化）
    void setViewport(int widthPixels, int heightPixels);

    /// Scene 可注入地形拾取链路；未注入或未命中时回退到控制器
    /// 内部的 WGS84 球面拾取。
    using SurfacePicker = std::function<bool(float xPixels, float yPixels, Vec3& outPoint)>;
    void setSurfacePicker(SurfacePicker picker);

    // 地形约束相关的类型与注入口全部归 CameraConstraintSolver；这里保留
    // 转发别名与转发方法，调用方（Scene 装配、宿主、测试）无需改动。
    using TerrainHeightFunc = CameraConstraintSolver::TerrainHeightFunc;
    using TerrainSample = CameraConstraintSolver::TerrainSample;
    using TerrainAreaSampleFunc = CameraConstraintSolver::TerrainAreaSampleFunc;

    void setTerrainHeightFunc(TerrainHeightFunc func);
    void setTerrainAreaSampleFunc(TerrainAreaSampleFunc func);
    /// 地形数据代次（瓦片集变更计数的代理）。变化 ⇒ 探针缓存失效。
    void setTerrainRevisionFunc(std::function<uint64_t()> func);

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
    /// 核心不变量：一个事件内只有锚点钉合(pin)产生横向世界运动，dolly/twist/
    /// pitch 在数学上全部严格保锚，与 pan 正交——没有意图分类。
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

    /// 时间步进（更新惯性动画）
    /// @param deltaSeconds 上一帧到现在的秒数
    void update(double deltaSeconds);

    /// 北极星测量台冻结开关：置 true 后 update() 变成完全空操作——不跑惯性、
    /// 不跑 zoom 惯性、不做 orbit 重建、不碰相机。相机停在最近一次显式
    /// lookAt/viewDistance 设定的位姿上，逐帧字节稳定，让重载耦合态（高空 + 深
    /// 影像 churn）下的 far 位姿也精确可复现（对拍去耦前后必须同位姿）。
    /// 启用时顺带清零所有惯性状态，避免冻结瞬间残留速度被"锁"进去。
    void setMeasurementFreeze(bool frozen);
    bool measurementFrozen() const { return measurementFreeze_; }

    /// 北极星测量台脚本化确定性平移(净测 §14.1② live 换页 ghost):置 active 后
    /// update() 每帧对当前视线施加固定 yawPerFrameRad 的"原地偏航"(绕相机所在
    /// 局部垂直轴,eye 不动、方位角扫掠 → 持续 page-in),共 frames 帧后 hold。
    /// 帧计数为内部计数器(每次 update 递增)故轨迹确定性、与 wall-clock/掉帧无关;
    /// 无惯性介入。与 measurementFreeze 互斥(freeze 优先)。free swipe 惯性漂不可控,
    /// 此脚本给可复现的受控运动,配 PageDet/截图量 ghost/stall。
    void setScriptedPan(bool active, int startFrame, int frames,
                        double yawPerFrameRad);
    bool scriptedPanActive() const { return scriptedPanActive_; }

    /// pan 惯性的"已经停了"阈值(rad/s)。低于它不再产生任何位移,故等价于零。
    /// **三处必须同用这一个常量**:运动闸、自清零、isSelfAnimating 判据。
    /// 各写各的字面量正是这类 bug 的温床 —— 判据比消费者宽一点点,表现就是
    /// 「拖一下之后永远不再空闲」,而画面上完全看不出区别。
    static constexpr double kMinInertiaAngularVelocity = 0.0001;

    /// 相机是否仍在自行演进(与外部输入无关的持续变化)。帧级按需渲染据此判定
    /// 「停手之后还得再画几帧」—— 惯性滑行/脚本平移期间画面每帧都在变,停帧
    /// 会把滑行冻在半途。
    ///
    /// ⚠️ 只报**自主演进**,不报「手指正按着」:后者由输入事件置事件型脏位,
    /// 两条路径分开才不会出现「手指不动但按着 → 既无事件又无自主演进 → 判定
    /// 空闲」这种两边都不认领的缝。
    bool isSelfAnimating() const {
        return scriptedPanActive_ || hasZoomInertia_ ||
               inertiaAngularVelocity_ > kMinInertiaAngularVelocity;
    }

    // ---- 相机状态 ----
    //
    // 位姿的唯一真值是 Camera 自身（eye/direction/up）。历史上还并存过一套
    // orbit 表示（rotation_ + distance_ + orbitMode_ 每帧 lookAt 地心重建），
    // 它是「缺 viewpoint API 时代」的位姿设定替代品，已整体删除：双表示不仅
    // 是冗余，还直接产出过「双击天空 → setDistance → 翻 orbit → 下一帧重建
    // 强制看向地心 → 丢弃全部 tilt」的视角瞬移。

    /// 相机地心距（地球半径单位）。**纯派生只读视图**（= |eye|/R），不是状态。
    /// 过渡接口：阶段 2b 引入 `currentViewpoint()` 后退役。
    float distance() const;

    /// 相机朝向的四元数表述：把 (+Z,+Y) 转到 (direction, up) 的旋转。
    /// **纯派生只读视图**，不是状态——历史上这是 orbit 表示的内部真值之一，
    /// 且它会与位姿脱节（`viewDistance`/构造函数改位姿却不改它）。派生版本
    /// 恒与位姿一致。过渡接口：阶段 2b 由 `currentViewpoint()` 取代。
    glm::dquat rotation() const;

    /// 把相机放到"目标点正上方 heightMeters、正北朝上、看向地心(nadir)"。
    /// 注意 eye 取自地心沿大地法线 (|target| + h) 处、视线指向**地心**——这是
    /// 原 orbit 语义的原样保留（大地法线不过地心，故 eye 并不严格在 target
    /// 正上方；差异在 ~11' 量级）。
    /// @param targetEcef      目标地表点(ECEF)
    /// @param surfaceUpNormal 目标点的大地法线(调用方从椭球取,本类不依赖
    ///                        Ellipsoid;传入无需归一化)
    /// @param heightMeters    相机在目标点上方的高度(米)
    void setNadirOrbitView(const Vec3& targetEcef,
                           const Vec3& surfaceUpNormal,
                           double heightMeters);

    /// 保持当前 target→eye 方位，把相机放到 target 外指定距离并看向 target。
    void viewDistance(const Vec3& targetWorld, double distanceMeters);

    /// 当前手势锚点世界坐标(ECEF)。有活动 drag/pinch 锚点时返回 true 并
    /// 写出 outWorld；否则返回 false。测试用查询（锚点获取/重试行为断言）。
    bool debugAnchorWorld(Vec3& outWorld) const;

    /// 相机相对地形的一次解算快照，resolveConstraints 每次刷新。纯读，
    /// 供渲染层（动态 near）、测试与诊断消费，不含策略。
    using CameraGroundState = CameraConstraintSolver::GroundState;
    const CameraGroundState& groundState() const {
        return constraintSolver_.groundState();
    }

    // 碰撞净空 ↔ 动态 near 的耦合契约（含 static_assert）归 solver 持有；
    // 这里转发，动态 near 的消费方（SceneFrameUpdateCoordinator）不需改动。
    static constexpr double kMinClearanceMeters =
        CameraConstraintSolver::kMinClearanceMeters;
    static constexpr double kNearFloorMeters =
        CameraConstraintSolver::kNearFloorMeters;
    static constexpr double kNearSafetyRatio =
        CameraConstraintSolver::kNearSafetyRatio;

    /// 相机方位角（弧度，0 = 正北，顺时针为正）。用于指北针。
    double headingRadians() const;
    /// 相机俯仰角（弧度，0 = 水平，-π/2 = 正俯视）。
    double pitchRadians() const;
    /// 复位到正北朝上（heading = 0），保持屏幕中心地物居中与当前俯仰。
    void resetNorthUp();

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
    void applyRotationAroundAxis(const glm::dvec3& axis, double angle);
    void rotateCameraAroundPoint(const glm::dvec3& center,
                                 const glm::dvec3& axis,
                                 double angle);
    /// @return 是否实际施加（被守卫拒绝时返回 false，供 Pitch 反 wind-up）
    bool rotateCameraVerticalAroundPoint(const glm::dvec3& center,
                                         double angle,
                                         double minSlope);
    /// 高空 zoom-out 回中（预算松弛，见 .cpp 常量说明）：手势/滑行期只充值
    /// 预算（不动相机，与锚点钉合严格正交），松手后 update() 指数消费。
    void accrueRecenterBudget(double zoomOutLogStep);
    void consumeRecenterBudget(double deltaSeconds);
    void applyCameraRotation(const glm::dquat& delta);

    /// 手势/惯性路径的位姿钳位：调用方刚显式动过相机，故恒 user-driven、
    /// 无帧间隔（dt=0）。与帧末哨兵是**两条性质不同的路径**——前者是"我刚动了，
    /// 钳一下"，后者是"检查有没有人绕过我"——别再合并回一个带 source 枚举的
    /// 函数：那样每个调用点都要现场装配一个 context，而枚举唯一的作用只是区分
    /// 是不是帧末。
    /// @param pinnedAnchorWorld 非空 → 碰撞解算沿 eye→anchor 直线退出（严格
    ///        保锚；径向抬升会泄漏 anchorErr）。指针仅在调用栈内有效。
    /// @return 位姿是否被修改
    bool clampNow(const glm::dvec3* pinnedAnchorWorld);

    /// 帧末哨兵：兜底收编所有未经 clampNow 路由的位姿写入（viewDistance /
    /// setNadirOrbitView / 回中 / scriptedPan / Facade/JNI 绕过控制器的裸写）。
    /// 靠位姿指纹判定 user-driven；冻结时完全不触碰（位姿须逐帧字节稳定）。
    /// @return 位姿是否被修改
    bool resolveAtFrameEnd(double deltaSeconds);

    /// 把解算落定的位姿提交给 solver（同时作扫掠基准与帧末指纹）。
    void commitResolvedPose();

    /// update() 的原函数体（惯性/回中）；帧末哨兵在 update() 包装层。
    void updateInternal(double deltaSeconds);

    Camera* camera_;
    SurfacePicker surfacePicker_;
    // 地形探针/突变滤波/碰撞钳位/groundState 全部归它；本类只在
    // resolveConstraints 这一个出口调用它。
    CameraConstraintSolver constraintSolver_;
    int viewportWidth_ = 1;
    int viewportHeight_ = 1;

    // 测量台冻结：true 时 update() 完全空转（见 setMeasurementFreeze）。
    bool measurementFreeze_ = false;

    // 测量台脚本化平移(见 setScriptedPan):active 时 update() 每帧原地偏航一步,
    // 内部帧计数确定性驱动,frames 帧后 hold。
    bool scriptedPanActive_ = false;
    int scriptedPanStartFrame_ = 0;  // 扫掠前先 hold 的帧数(让冷启动 settle)
    int scriptedPanFrames_ = 0;
    int scriptedPanFrame_ = 0;
    double scriptedPanYawPerFrameRad_ = 0.0;

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
    // 是固定世界点，pan 惯性转的是相机（applyCameraRotation），世界点不动，
    // 双惯性并行无需同转锚点——dolly 朝固定世界点在任意相机旋转下都正确。
    glm::dvec3 pinchPanAxis_{0.0, 1.0, 0.0};
    double pinchPanAngularVelocity_ = 0.0;

    // 高空回中欠账（弧度）。手势/滑行期充值，无手势时 update() 消费。
    double recenterBudgetRadians_ = 0.0;

    // zoom 惯性状态（对数距离空间，见 .cpp 常量说明）
    bool hasZoomInertia_ = false;
    double zoomInertiaLogRate_ = 0.0;         // d(ln dist)/dt，>0 = 继续拉近
    glm::dvec3 zoomInertiaAnchor_{0.0, 0.0, 0.0};

};

} // namespace earth_engine
