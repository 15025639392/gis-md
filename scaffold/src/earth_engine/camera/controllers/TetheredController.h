#pragma once

#include "../Viewpoint.h"
#include "ICameraController.h"
#include "TouchGesture.h"

#include <glm/glm.hpp>

namespace earth_engine {

class Camera;
class CameraConstraintSolver;

/// 系留控制器:相机固连到一个由 provider 给出的参考系(载体/目标)。
///
/// ⚠️⚠️ **真值是 (frame, localHPR, range),不是世界位姿**。这是整套架构的根决策
/// (见 `docs/camera-system-architecture.md` §2):载体动了而世界位姿不动就等于脱钩,
/// 所以世界位姿在这里是**每帧派生出来的量**,反过来才对。
/// `FreeGlobeController` 恰好相反——那里真值是世界位姿,因为掠视/视线不交地面时
/// focal 根本不存在,用 (focal,hpr,range) 表述会在我们做得最对的区间里病态。
/// **同一个系统里两种真值并存,正是它们必须是两个控制器的原因。**
///
/// 参考系三档(`ViewpointFrame` 的两个 provider):
/// | origin | orientation | 语义 |
/// |---|---|---|
/// | 空 | 空 | 退化成绕地心某固定点的 orbit(基本没用,但不崩) |
/// | 有 | 空 | 跟踪目标位置,视角仍按地理 ENU(跟车但保持北上) |
/// | 有 | 有 | 完全固连载体机体系(座舱视角,roll 跟随载体) |
///
/// ⚠️ **净空不豁免**:本控制器每帧从真值重算世界位姿,`CameraSystem` 的帧末哨兵
/// 随后钳位。于是**渲染出来的每一帧都是钳过的,而真值保持未钳**——载体离开地形后
/// 相机精确回到原来的相对位姿,不留下被地形推走的欠账。这个"真值不钳、派生量钳"
/// 的分工是 tether 语义能成立的关键,别改成钳完写回真值。
///
/// 非目标(明确不做,不是遗漏):载体姿态的平滑/滞后滤波(Skybolt 的
/// `orientationLagTimeConstant`)。真实载体姿态带噪声时相机会抖,那是产品调校,
/// 需要真机手感判断,不该在没有device 验证的情况下先塞一个常量进来。
class TetheredController final : public ICameraController,
                                 public ITouchGestureTarget {
public:
    TetheredController(Camera* camera, CameraConstraintSolver* solver);

    /// 设定参考系。切换 frame **不改 localHPR/range** —— 那正是"相对位姿"的语义:
    /// 从跟 A 改成跟 B,相机保持同样的相对姿态与距离。
    void setFrame(ViewpointFrame frame);
    const ViewpointFrame& frame() const { return frame_; }

    // ---- 真值读写 ----
    void setLocalOrientation(double headingRadians,
                             double pitchRadians,
                             double rollRadians);
    void setRange(double rangeMeters);
    double localHeading() const { return heading_; }
    double localPitch() const { return pitch_; }
    double localRoll() const { return roll_; }
    double range() const { return range_; }

    /// 参考系是否解算成功过(provider 返回 false / 未设 frame ⇒ false)。
    bool frameResolved() const { return frameResolved_; }

    void setViewport(int widthPixels, int heightPixels) override;

    // ---- ICameraController ----
    void tick(double deltaSeconds) override;
    /// 接管:把**当前世界位姿**换算成本参考系下的 (localHPR, range)。
    /// 这是真值分离能无缝互换的全部机制。
    /// ⚠️ **位置精确保留,视线会重新对准载体**:真值是 orbit 表述
    /// (eye = origin − direction·range),表达不了"在载体附近却看着别处"的位姿,
    /// 位置与朝向只能保一个。原本就看着载体时(正常用法)两者同时保住。
    void onActivate() override;
    void onDeactivate() override;
    /// 载体在动就还得继续画。⚠️ 判据是"参考系相对上一帧变了",不是恒 true——
    /// 恒 true 会让静止的系留相机永远不空闲,按需渲染彻底失效。
    bool isAnimating() const override { return frameMoving_; }

    // ---- ITouchGestureTarget ----
    // 语义与 Free 完全不同:这里拖拽是**绕载体转**(载体在屏幕中心不动),
    // 没有"抓住地表点跟手"的锚点数学——载体在动,地表锚点当场失去意义。
    void onDragStart(float xPixels, float yPixels, double timestamp) override;
    void onDragMove(float xPixels, float yPixels, double timestamp) override;
    void onDragEnd() override;
    void onPinchGesture(const PinchInput& input) override;
    void onPinchEnd() override;
    bool pinching() const override { return pinching_; }

    /// 俯仰上下限(弧度)。留出余量避免在 ±π/2 的万向节点上打转。
    static constexpr double kMaxPitchRadians = 1.5533;   // ≈ 89°
    static constexpr double kMinRangeMeters = 0.0;

private:
    /// 解析参考系。@return false = originProvider 说目标暂不可用 ⇒ 保持上帧。
    bool resolveFrame(glm::dvec3& outOrigin, glm::dmat3& outBasis) const;
    /// 由真值写出世界位姿。
    void applyPose(const glm::dvec3& origin, const glm::dmat3& basis);
    /// 把当前世界位姿换算成本参考系下的真值(接管对齐)。**保位置、重新对准
    /// 载体**——理由见 .cpp 里那段 ⚠️(orbit 表述下位置与朝向只能保一个)。
    void alignToCurrentPose(const glm::dvec3& origin, const glm::dmat3& basis);

    Camera* camera_;
    CameraConstraintSolver* solver_;
    ViewpointFrame frame_;

    // 真值。
    double heading_ = 0.0;
    double pitch_ = 0.0;
    double roll_ = 0.0;
    double range_ = 0.0;

    bool frameResolved_ = false;
    bool frameMoving_ = false;
    bool hasPreviousFrame_ = false;
    glm::dvec3 previousOrigin_{0.0};
    glm::dmat3 previousBasis_{1.0};

    int viewportWidth_ = 1;
    int viewportHeight_ = 1;

    bool dragging_ = false;
    float dragLastX_ = 0.0f;
    float dragLastY_ = 0.0f;

    bool pinching_ = false;
    double pinchAppliedScaleLog_ = 0.0;
};

} // namespace earth_engine
