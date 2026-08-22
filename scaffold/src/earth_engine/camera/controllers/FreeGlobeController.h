#pragma once

#include "../../core/math/Vec3.h"
#include "../../interaction/InputEvent.h"
#include "ICameraController.h"
#include "TouchGesture.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <functional>
#include <vector>

namespace earth_engine {

class Camera;
class Ray;
class CameraConstraintSolver;

/// 球面（Free 模式）手势操控器：把触摸事件翻译成相机位姿变化。
///
/// 分层位置：**输入 → 位姿** 这一层。它不拥有相机，也不拥有约束求解器，
/// 只持有二者的指针；每次改完位姿立刻走 `clampNow` 过一遍约束出口。
/// 编排层（CameraSystem）负责帧循环、帧末哨兵、视点设定与只读派生量。
///
/// 为什么独立成类：后面还会有语义完全不同的第二、第三个实现——Tethered
/// 模式（拖拽=绕载体转，不是绕地心转）与桌面输入（滚轮/中键/方向键，事件
/// 形状不同但共用锚点数学）。焊在一个类里就只能靠 `if (mode == ...)` 把两套
/// 数学缝进同一个函数体。
///
/// 核心不变量（与 CameraSystem 时代逐字保留）：
/// - 单指拖拽先抓取地表点，移动时让该点尽量跟随手指；
/// - 双指手势中**只有锚点钉合(pin)产生横向世界运动**，dolly/twist/pitch 在
///   数学上全部严格保锚，与 pan 正交——没有意图分类。
class FreeGlobeController final : public ICameraController,
                                  public ITouchGestureTarget {
public:
    /// @param camera 受控相机（非空，生命周期由调用者管理）
    /// @param solver 约束求解器（非空，由编排层拥有）
    FreeGlobeController(Camera* camera, CameraConstraintSolver* solver);

    /// 设置视口尺寸（用于 pick ray 和屏幕坐标归一化）
    void setViewport(int widthPixels, int heightPixels) override;

    /// Scene 可注入地形拾取链路；未注入或未命中时回退到内部的 WGS84 球面拾取。
    using SurfacePicker = std::function<bool(float xPixels, float yPixels, Vec3& outPoint)>;
    void setSurfacePicker(SurfacePicker picker);

    /// drag 开始（手指按下）
    /// @param timestamp 单调时钟时间戳（秒），用于惯性角速度计算
    void onDragStart(float xPixels, float yPixels, double timestamp) override;

    /// drag 移动（手指滑动）
    void onDragMove(float xPixels, float yPixels, double timestamp) override;

    /// drag 结束（手指抬起，启动惯性）
    void onDragEnd() override;
    /// drag 被取消：立即停、清惯性、锚点作废（契约 1.5）。
    void onDragCancel() override;

    void onPinchGesture(const PinchInput& input) override;
    void onPinchEnd() override;
    /// pinch 被取消：立即停、不启动 zoom 惯性（契约 2.3）。
    void onPinchCancel() override;
    bool pinching() const override { return pinching_; }
    /// 键盘命令（契约 3.3，Mapbox 键位）：方向键平移 100px、+/- 缩放 ±1 级
    /// （Shift 加倍 ±2 级）、Shift+方向键旋转 15°/倾斜 10°。
    void onKeyCommand(InputEvent::Key key,
                      const InputEvent::Modifiers& modifiers);

    /// 惯性/滑行/回中的时间步进。编排层在冻结与脚本平移的早退**之后**调用，
    /// 故本函数不认识这两种测量台状态。
    void tick(double deltaSeconds) override;

    /// 接管：本控制器的全部状态都是**手势期瞬时量**（抓取点/锚点/惯性/回中
    /// 欠账），没有一样需要从位姿反解 —— 位姿本身就是唯一真值，读它即可。
    /// 故对齐 = 把这些瞬时量清空，避免上一段手势的残留在接管后继续自走。
    void onActivate() override;
    /// 交出：同上，瞬时量不跨控制器存活。
    void onDeactivate() override;

    /// 惯性速度的"归零"最小量(rad/s)。tick() 里的真实停止判据是"折合屏幕
    /// 位移 < 0.5px/帧"(Cesium 规则,视口相关);此常量只是数值归零的 epsilon,
    /// 运动闸、自清零、isAnimating 判据三处必须同用这一个常量。
    static constexpr double kMinInertiaAngularVelocity = 0.0001;

    /// 是否仍在自行滑行（惯性/zoom 滑行）。**不含**编排层的脚本平移。
    bool isAnimating() const override {
        return hasZoomInertia_ ||
               inertiaAngularVelocity_ > kMinInertiaAngularVelocity ||
               nearInertiaActive_ || zoomSettleActive_;
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
    /// 近地拖图/惯性的等效手指(起手 + 已施加偏移;诊断/测试)。非近地路径
    /// 返回 false —— 爬升保锚断言按等效手指比,不被地平线裁剪的 raw 差污染。
    bool debugNearEffectiveFinger(float& fx, float& fy) const;

private:
    /// CAMPROBE 诊断:在手势 START/每 MOVE/END 各吐一行 logcat(tag=CAMPROBE),含
    /// 手指像素 + viewport + 当前锚点世界坐标(ECEF)+ view·proj 矩阵(16
    /// doubles,列主序)。主机侧用 VP 投影 anchorWorld 得"实际落点",对比注入
    /// 的手指像素("本该落点")→ 逐手势 anchorErr / 增益,把 pin 正确性从观感
    /// 变机制自证。纯 dump,不改任何相机/手势行为。
    void logCameraProbe(const char* phase, double fingerX, double fingerY) const;

    /// 单指拖拽反馈模式（契约 1.2）：起手判定一次，整段拖拽不切换。
    /// Space = 空间拖球（绕地心锚点旋转）；NearGround = 近地拖图（姿态锁定、
    /// 锚点钉在指下、Δpx 等量换算地表位移）。
    enum class DragMode : uint8_t {
        Space,
        NearGround
    };
    /// 模式判据（对齐 Cesium spin3D）：海拔 < 150km（Cesium
    /// minimumPickingTerrainHeight）且起手拾取射线与地表**掠射**
    /// （|dot(rayDir, 地表法线)| < 0.05）或相机在拾取点内侧 → strafe 近地
    /// 拖图；否则空间拖球（绕地心旋转）。不再用 pitch≥60° 硬阈值。
    DragMode resolveDragMode(float xPixels, float yPixels) const;

    /// 近地像素平移（契约 1.3）：锚点钉在指下，姿态完全锁定；每帧把屏幕
    /// Δpx 经"锚点局部切平面"换算成世界位移（旋转的平直极限，不引入第二套
    /// 相机数学）；位移/惯性偏移 ≤ 0.75×地平线像素距离（MapLibre PR #6345），
    /// 越界即停、绝不反向。
    void applyNearGroundPan(float xPixels, float yPixels, double timestamp);
    /// 近地惯性：松手后按 iOS 衰减（v *= 0.998/ms）沿锁定方向滑行，
    /// 停止判据 = 折合屏幕位移 < 0.5px/帧（与空间模式同规则）。
    void tickNearGroundInertia(double deltaSeconds);
    /// 爬升保锚（2026-08-20,C-V1 扩展）：clampNow 在近地掠射下因 eye→anchor
    /// 近水平退径向抬升,锚点像素被抬离等效手指(真机 anchorErr 峰 374px)。
    /// 用抬升后的相机重投影补正(平移量=锚点−射线∩切平面),再 clamp 收残差,
    /// 相机沿墙"边进边升";每帧爬升预算由 constrainEye 共享(25m/帧),循环不
    /// 叠加弹跳。最多 2 轮,残差几何收缩,超界即停;真穿地紧急抬满路径 C-V6
    /// 优先,锚点临时让位。
    void repinNearGroundAnchor(float fX, float fY,
                               const glm::dvec3& planeNormal,
                               const glm::dvec3& anchorWorld);
    /// 键盘平移（契约 3.3）：空间=转台旋转，近地=切平面平移（带地平线裁剪）。
    void panByPixels(double dx, double dy);
    /// 键盘缩放：绕屏幕中心锚点 ±levels 缩放级（1 级 = ×2）。
    void zoomByLevels(double levels);
    void rotateHeadingByDegrees(double degrees);
    void pitchByDegrees(double degrees);

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
    /// 松手惯性的视差增益:采样/判停用"视角角速度"(手指感知,契约 1.4 手感
    /// 量),应用旋转用"绕地心角速度" = 视角率 × 增益。增益 = |eye−anchor|/|eye|
    /// (低空锚点距相机仅 ~km 级,相对地心距 ~1/4000,直接采绕地心率会让松手
    /// 惯性第一帧就被 0.5px/帧 判停);无锚(spin/转台回退)时转台旋转本身就是
    /// 视角旋转,增益=1。
    double inertiaGain_ = 0.0;
    /// 高空球心回中(契约 2.4):拉远到地球可见后,视轴按高度平滑转向地心,让
    /// 球心自然回到屏幕中心(缩放几何的一部分,不是松手后的事后补偿)。近地
    /// (<1.5R)不干预,保持锚点钉指;4R 以上完全对准地心。只由缩放路径调用
    /// (捏合拉远/滚轮 settle/zoom 惯性),不与平移/倾斜抢方向。
    void blendViewTowardGlobeCenter();
    double lastDragTimestamp_ = 0.0;  // 最近一次 drag 事件的时间戳
    /// iOS 风格速度采样（契约 1.4）：最近 ≤3 个相邻样本，松手时按
    /// 0.6/0.35/0.05 加权（最新样本权重最低——它是"松开前一刻"的抖动值）。
    struct DragVelocitySample {
        double dt = 0.0;
        glm::dvec3 axis{0.0, 1.0, 0.0};
        double rate = 0.0;  // rad/s（已按上限钳位）
    };
    std::vector<DragVelocitySample> dragVelocitySamples_;

    // 近地拖图状态（契约 1.2/1.3）
    DragMode dragMode_ = DragMode::Space;
    bool nearInertiaActive_ = false;
    Vec3 nearAnchorWorld_{0.0, 0.0, 6378137.0};
    Vec3 nearAnchorNormal_{0.0, 0.0, 1.0};
    /// 惯性期 CAMPROBE 保留锚点(诊断):onDragEnd 存最后抓取点,惯性活跃帧由
    /// debugAnchorWorld 返回,让探针能算真实锚点屏幕速度(2026-08-20 判
    /// H-G1/H-G2)。不进任何生产逻辑。
    Vec3 debugInertiaAnchorWorld_{0.0, 0.0, 6378137.0};
    bool debugHasInertiaAnchor_ = false;
    float nearStartX_ = 0.0f;   // 起手手指像素（锚点初始指下位置）
    float nearStartY_ = 0.0f;
    double nearAppliedOffsetX_ = 0.0;     // 已施加的屏幕偏移（相对起手）
    double nearAppliedOffsetY_ = 0.0;
    double nearVelocityX_ = 0.0;          // 近地惯性速度（px/s，屏幕系）
    double nearVelocityY_ = 0.0;
    struct PixelVelocitySample {
        double dt = 0.0;
        float dx = 0.0f;
        float dy = 0.0f;
    };
    std::vector<PixelVelocitySample> pixelVelocitySamples_;

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
    // 上一事件质心（pin 病态区转台回退的位移基准）。
    float lastPinchCentroidX_ = 0.0f;
    float lastPinchCentroidY_ = 0.0f;
    // zoom 惯性状态（对数距离空间，见 .cpp 常量说明）
    bool hasZoomInertia_ = false;
    double zoomInertiaLogRate_ = 0.0;         // d(ln dist)/dt，>0 = 继续拉近
    glm::dvec3 zoomInertiaAnchor_{0.0, 0.0, 0.0};

    // 滚轮平滑缩放（契约 3.1）：目标/已施加的对数缩放（累计），tick 指数收敛
    // （~300ms），单帧上限 ±ln(2)（Mapbox maxScalePerFrame=2），不越目标（不反向）。
    bool zoomSettleActive_ = false;
    double zoomSettleTargetLog_ = 0.0;
    double zoomSettleAppliedLog_ = 0.0;
    glm::dvec3 zoomSettleAnchor_{0.0, 0.0, 0.0};
    static constexpr double kZoomSettleRatePerSecond = 8.0;  // ~0.3s 收敛 91%
    static constexpr double kMaxZoomLevelsPerFrame = 1.0;    // ±1 级/帧
};

} // namespace earth_engine
