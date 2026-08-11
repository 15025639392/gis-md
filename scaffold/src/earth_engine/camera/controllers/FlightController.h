#pragma once

#include "../CameraPose.h"
#include "ICameraController.h"

#include "../../core/geodesy/SimplePlanarEllipsoidCurve.h"

#include <glm/glm.hpp>
#include <optional>

namespace earth_engine {

class Camera;
class CameraConstraintSolver;

/// 程序化飞行控制器:沿椭球平面曲线从当前位姿飞到目标位姿。
///
/// 与 `FreeGlobeController` 并列,由 `CameraControllerSelector` 二选一驱动。
/// **不吃输入**——这正是 `ICameraController` 接口里没有输入的理由之一。
///
/// ⚠️ 三条最容易漏的耦合(全部在这一层之外,见 `CameraSystem` 与 `FrameState`):
/// 1. 瓦片系统的 `cameraMoving` 纯按位移判、不区分驱动源 ⇒ 飞行期恒真 ⇒
///    `cullRequestsWhileMoving` 全程延迟请求 ⇒ **飞到目的地画面是空的**。
///    减速段必须关掉它(Cesium 用 `Camera.canPreloadFlight()` 解同一件事)。
/// 2. **净空不豁免**。飞行不是"程序在动所以可以穿地"的理由——它靠规划期沿路径
///    采地形把拱高抬够,使碰撞钳位**结构性不触发**;真触发了帧末哨兵照样钳。
/// 3. `isSelfAnimating()` 必须含飞行期,否则"飞到一半停帧冻住"(与当初漏 pan
///    惯性同坑)。这条由 `isAnimating()` 自动满足。
class FlightController final : public ICameraController {
public:
    FlightController(Camera* camera, CameraConstraintSolver* solver);

    /// 规划并启动一次飞行。
    /// @param from 起点位姿(通常 = 当前相机位姿)
    /// @param to   终点位姿(由 `CameraSystem::resolveViewpoint` 解出)
    /// @param durationSecondsOverride >0 时覆盖时长启发式;<=0 用启发式
    /// @return 起终点几乎重合 / 曲线退化 ⇒ false(**不进入飞行态**,调用方应直接
    ///         落位。返回 true 才代表接下来会有 tick 驱动)
    bool start(const CameraPose& from,
               const CameraPose& to,
               double durationSecondsOverride);

    void cancel();

    bool active() const { return active_; }
    /// 归一化进度 [0,1](时间轴,**未过缓动**)。瓦片侧的减速段判据用它。
    double progress() const { return progress_; }
    double durationSeconds() const { return durationSeconds_; }
    /// 规划出的拱高(米)。测试与诊断用。
    double archHeightMeters() const { return archHeightMeters_; }

    /// 本次 tick 是否刚好飞完(**取走即清**)。`CameraSystem` 据此做落点交接:
    /// 精确落到终点位姿并把驱动权交还 Free —— 靠插值自然收敛到终点会留下
    /// 与缓动实现相关的残差,而验收判据是终点位姿相对误差 < 1e-3。
    bool consumeCompleted();

    // ---- ICameraController ----
    void tick(double deltaSeconds) override;
    bool isAnimating() const override { return active_; }
    /// ⚠️ **不清飞行计划**:计划由 `start()` 在 `select()` 之前建立,接管时清掉
    /// 就等于刚起飞就被自己取消。这是本控制器与 `FreeGlobeController` 的关键
    /// 差异——后者的状态全是手势期瞬时量,接管时清空才对。
    void onActivate() override {}
    void onDeactivate() override { cancel(); }

private:
    /// 沿路径采地形,返回需要的拱高(米):max(看见两端所需, 地形最高 + 净空, 0)。
    double planArchHeight(const glm::dvec3& fromEcef,
                          const glm::dvec3& toEcef,
                          const SimplePlanarEllipsoidCurve& curve) const;

    Camera* camera_;
    CameraConstraintSolver* solver_;

    bool active_ = false;
    bool completed_ = false;
    double elapsedSeconds_ = 0.0;
    double durationSeconds_ = 0.0;
    double progress_ = 0.0;
    double archHeightMeters_ = 0.0;

    std::optional<SimplePlanarEllipsoidCurve> curve_;
    CameraPose from_;
    CameraPose to_;
    // 起终朝向(各自在自己 eye 处的 ENU 里)。heading 已 unwrap 成最短弧。
    double fromHeading_ = 0.0, fromPitch_ = 0.0, fromRoll_ = 0.0;
    double toHeading_ = 0.0, toPitch_ = 0.0, toRoll_ = 0.0;
};

} // namespace earth_engine
