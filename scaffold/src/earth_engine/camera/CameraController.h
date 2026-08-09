#pragma once

#include "../core/math/Vec3.h"
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

    /// Returns height above WGS84 ellipsoid (meters) at the given ECEF position,
    /// or nullopt when no terrain data covers the point (unloaded / no coverage).
    /// When set, collision clamping uses terrain height instead of bare ellipsoid;
    /// on nullopt the clamp holds the last known terrain height rather than
    /// treating the point as sea level (which would let the eye sink into
    /// not-yet-loaded terrain).
    using TerrainHeightFunc =
        std::function<std::optional<double>(const Vec3& ecefPosition)>;
    void setTerrainHeightFunc(TerrainHeightFunc func);

    /// 近场区域批量地形采样注入口（碰撞探针/动态 near 数据源）。以
    /// groundEcef 为中心，对本地 ENU 偏移（米，x=东 y=北）逐点采样，out 与
    /// offsets 等长，无覆盖点 valid=false。实现方保证渲染网格一致采样——
    /// 相机不能穿的是上屏的那张面，不是全分辨率理论面。注入后碰撞钳位
    /// 改用探针（内环+扫掠走廊最大高），TerrainHeightFunc 退为无探针回退。
    struct TerrainSample {
        bool valid = false;
        Vec3 surfaceEcef;      ///< 地形点（含高度）ECEF
        double heightMeters = 0.0;  ///< 椭球高
    };
    using TerrainAreaSampleFunc = std::function<void(
        const Vec3& groundEcef,
        double radiusMeters,
        const std::vector<glm::dvec2>& localOffsetsMeters,
        std::vector<TerrainSample>& out)>;
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

    /// 设置相机到地球中心的距离（地球半径单位，默认 7.0）
    void setDistance(float earthRadii);
    float distance() const { return distance_; }

    /// 获取当前旋转四元数
    const glm::dquat& rotation() const { return rotation_; }

    /// 直接设置旋转
    void setRotation(const glm::dquat& q);

    /// 把 orbit 状态设为"目标点正上方 heightMeters、正北朝上、看向地心
    /// (nadir)"。orbit 约定(eye = -(rotation_·+Z)·distance_·R,
    /// up = rotation_·+Y)是本类的私有实现细节:外部调用方(如
    /// EarthEngineSdkFacade::resetCamera)一律走本接口,不得在类外复刻
    /// 四元数推导——约定变更只需要改本类。
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
    struct CameraGroundState {
        /// 是否已至少解算过一次（false ⇒ 消费方退回各自的旧公式）。
        bool valid = false;
        /// 滤波后的近场地形高（椭球高，米）。高空快速路径不采样时保持上值。
        double terrainHeightMeters = 0.0;
        /// AGL = 相机椭球高 − terrainHeightMeters。
        double heightAboveTerrain = 0.0;
        /// eye 到近场地形几何的保守最小三维距离（米）：探针采样最小距离与
        /// "盘外墙"下界（盘外地形水平偏移 ≥ R_probe、高度 ≤ 9000m ⇒ 距离
        /// ≥ √(R²+max(0,H−9000)²)）取 min；高空快速路径 = 椭球高 − 9000；
        /// 无探针时退化为竖直 AGL。动态 near 消费。
        double nearestGeometryMeters = 0.0;
        /// 本次解算是否拿到了有效地形样本（false = 快速路径/无覆盖）。
        bool hasTerrainData = false;
    };
    const CameraGroundState& groundState() const { return groundState_; }

    /// 碰撞净空 ↔ 动态 near 的耦合契约（禁止单独改动其一）：净空保证
    /// "最近地形几何 ≥ kMinClearanceMeters"，near = Ratio×最近几何 ≥ Floor
    /// 才能既压住 z_ndc 病态区又不切脚下地面。
    static constexpr double kMinClearanceMeters = 50.0;
    static constexpr double kNearFloorMeters = 5.0;
    static constexpr double kNearSafetyRatio = 0.5;
    static_assert(kNearSafetyRatio * kMinClearanceMeters >= kNearFloorMeters,
                  "near 下限超过净空×安全比:近平面会切进脚下地面");

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
    /// 按当前模式施加旋转增量（orbit 模式转 rotation_，自由模式转相机整体）。
    void applyRotationDelta(const glm::dquat& delta);

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
    void syncDistanceFromCamera();

    /// 地形碰撞解算：把 eye 钳到滤波后地形高 + 视觉下限之上（仅抬升，不下压），
    /// 途中刷新探针/滤波与 groundState_。仅允许 resolveConstraints 调用。
    /// @param pinnedAnchorWorld 非空时退出方向沿 eye→anchor 直线（方向与
    ///        dir/up 全不变 ⇒ 锚点像素严格不动）；该直线近水平（后退换不来
    ///        高度）时退回大地法线径向抬升。
    glm::dvec3 constrainEyeAgainstTerrain(const glm::dvec3& eye,
                                          bool userDriven,
                                          double deltaSeconds,
                                          const glm::dvec3* pinnedAnchorWorld);
    /// 近场探针按需重建（中心漂移/半径变化/代次变化；每帧至多 1 次）。
    void refreshTerrainProbeIfNeeded(const glm::dvec3& eye,
                                     const Vec3& surface);
    /// 非对称地形突变滤波：用户驱动/上升/小变动立即，数据驱动大幅下降
    /// 按 τ 指数逼近（见 .cpp 常量说明）。
    void updateFilteredTerrainHeight(double rawHeightMeters,
                                     bool userDriven,
                                     double deltaSeconds);

    /// 相机位姿合法性的唯一出口（choke point）。手势/惯性路径在事件内调用；
    /// update() 帧末哨兵兜底收编所有未显式路由的位姿写入（orbit 重建、
    /// viewDistance/setNadirOrbitView、回中、scriptedPan、外部绕过控制器
    /// 的 Camera 裸写）。约束实现只允许改这里，禁止在调用点各自补钳位。
    struct ConstraintContext {
        enum class Source { Gesture, Inertia, FrameEnd };
        Source source = Source::FrameEnd;
        /// 非空 → 碰撞解算沿 eye→anchor 直线退出（严格保锚；径向抬升会
        /// 泄漏 anchorErr）。指针仅在调用栈内有效。
        const glm::dvec3* pinnedAnchorWorld = nullptr;
        /// true = 绝不修改位姿（measurementFreeze 的帧末哨兵：位姿必须
        /// 逐帧字节稳定，但地面状态仍可刷新供渲染层读取）。
        bool observeOnly = false;
        /// 帧间隔（秒）。仅数据驱动的滤波衰减消费；手势/惯性事件传 0 即可
        /// （它们是 user-driven，滤波恒立即）。
        double deltaSeconds = 0.0;
    };
    /// @return 位姿是否被修改
    bool resolveConstraints(const ConstraintContext& ctx);
    /// update() 的原函数体（惯性/回中/orbit 重建）；帧末哨兵在 update() 包装层。
    void updateInternal(double deltaSeconds);
    /// orbit 参数 (rotation_/distance_) → 相机位姿。update() 尾部与 orbit
    /// 模式的约束解算（钳 distance_ 后重跑）共用。
    void rebuildOrbitPose();

    Camera* camera_;
    SurfacePicker surfacePicker_;
    TerrainHeightFunc terrainHeightFunc_;
    // 滤波后的近场地形高(米)。无数据时保持现值(保守回退,防 dip→pop)；
    // 数据驱动的突变经非对称滤波(updateFilteredTerrainHeight)。
    double filteredTerrainHeight_ = 0.0;
    CameraGroundState groundState_;

    // 近场地形探针(区域批量采样缓存):同心环(旋转对称,near 口径=全部采样点
    // 三维最小距离)+扫掠走廊(单帧大位移跨越的山脊,碰撞口径)。每帧至多重建
    // 1 次,手势期高频事件共享同帧探针。
    TerrainAreaSampleFunc terrainAreaSampleFunc_;
    std::function<uint64_t()> terrainRevisionFunc_;
    struct TerrainProbe {
        bool valid = false;
        bool hasData = false;
        glm::dvec3 centerSurfaceEcef{0.0};
        double radiusMeters = 0.0;
        uint64_t revision = 0;
        /// 碰撞口径:内环(r≤0.15R)+扫掠走廊采样的最大地形高。
        double collisionMaxHeight = 0.0;
        /// near 口径:全部有效采样的三维地形点(动态 near 消费)。
        std::vector<glm::dvec3> samplePointsEcef;
    };
    TerrainProbe terrainProbe_;
    uint64_t frameIndex_ = 0;
    uint64_t lastProbeRebuildFrame_ = ~0ull;
    // 上次 resolveConstraints 通过后的位姿指纹：帧末不等 ⇒ 有人绕过控制器
    // 写了 Camera（Facade/JNI 裸写）,按 user-driven 处理（突变滤波消费）。
    bool hasLastResolvedPose_ = false;
    glm::dvec3 lastResolvedEye_{0.0};
    glm::dvec3 lastResolvedDir_{0.0};
    int viewportWidth_ = 1;
    int viewportHeight_ = 1;

    glm::dquat rotation_{1.0, 0.0, 0.0, 0.0};
    float distance_ = 7.0f;
    bool orbitMode_ = true;
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
