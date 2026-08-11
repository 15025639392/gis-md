#pragma once

#include "../core/math/Vec3.h"
#include "CameraConstraintSolver.h"
#include "GlobeGestureManipulator.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <functional>

namespace earth_engine {

class Camera;

/// 相机编排层：拥有约束求解器与操控器，负责帧循环、帧末哨兵、视点设定
/// 与只读派生量。
///
/// 分层：**输入 → 位姿**归 `GlobeGestureManipulator`（锚点数学/惯性/回中），
/// **位姿 → 合法位姿**归 `CameraConstraintSolver`（地形探针/突变滤波/碰撞
/// 钳位），本类是把二者接在一起的那一层。后续阶段这里会长出控制器
/// selector（Tethered/桌面输入各是一个并列的操控器），届时改名 CameraSystem。
class CameraController {
public:
    /// @param camera 受控相机（非空，生命周期由调用者管理）
    explicit CameraController(Camera* camera);

    /// 设置视口尺寸（用于 pick ray 和屏幕坐标归一化）
    void setViewport(int widthPixels, int heightPixels);

    /// Scene 可注入地形拾取链路；未注入或未命中时回退到 WGS84 球面拾取。
    using SurfacePicker = GlobeGestureManipulator::SurfacePicker;
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

    // ---- 手势输入（转发给操控器；起手帧先跑一个同步帧，见 .cpp）----
    //
    // 手势数学与惯性归 GlobeGestureManipulator，别名让调用方
    // （SceneInputCoordinator、测试）继续按 CameraController::PinchMode 书写。
    using PinchMode = GlobeGestureManipulator::PinchMode;
    using PinchInput = GlobeGestureManipulator::PinchInput;

    /// drag 开始（手指按下）
    /// @param timestamp 单调时钟时间戳（秒），用于惯性角速度计算
    void onDragStart(float xPixels, float yPixels, double timestamp = 0.0);
    /// drag 移动（手指滑动）
    void onDragMove(float xPixels, float yPixels, double timestamp = 0.0);
    /// drag 结束（手指抬起，启动惯性）
    void onDragEnd();

    /// 双指手势输入（绝对量表述：事件被合并/丢弃不产生累积漂移）。
    void onPinchGesture(const PinchInput& input);
    /// 旧契约薄适配器（音量键合成捏合 / 无 pointer pair 的平台）。迁移完成后删除。
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
    /// 不跑 zoom 惯性、不碰相机。相机停在最近一次显式 lookAt/viewDistance
    /// 设定的位姿上，逐帧字节稳定，让重载耦合态（高空 + 深影像 churn）下的
    /// far 位姿也精确可复现（对拍去耦前后必须同位姿）。
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

    /// 相机是否仍在自行演进(与外部输入无关的持续变化)。帧级按需渲染据此判定
    /// 「停手之后还得再画几帧」—— 惯性滑行/脚本平移期间画面每帧都在变,停帧
    /// 会把滑行冻在半途。
    ///
    /// ⚠️ 只报**自主演进**,不报「手指正按着」:后者由输入事件置事件型脏位,
    /// 两条路径分开才不会出现「手指不动但按着 → 既无事件又无自主演进 → 判定
    /// 空闲」这种两边都不认领的缝。
    bool isSelfAnimating() const {
        return scriptedPanActive_ || manipulator_.isAnimating();
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

    /// 相机相对地形的一次解算快照，每次约束解算刷新。纯读，供渲染层
    /// （动态 near）、测试与诊断消费，不含策略。
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
    /// 绕过地心的定轴旋转（脚本平移专用；轴/角退化时静默跳过）。
    void applyRotationAroundAxis(const glm::dvec3& axis, double angle);

    /// 帧末哨兵：兜底收编所有未经操控器 clampNow 路由的位姿写入
    /// （viewDistance / setNadirOrbitView / scriptedPan / Facade/JNI 绕过
    /// 控制器的裸写）。靠位姿指纹判定 user-driven；冻结时完全不触碰
    /// （位姿须逐帧字节稳定）。
    /// @return 位姿是否被修改
    bool resolveAtFrameEnd(double deltaSeconds);

    /// 把解算落定的位姿提交给 solver（同时作扫掠基准与帧末指纹）。
    void commitResolvedPose();

    /// update() 的原函数体（测量台早退 + 操控器 tick）；帧末哨兵在
    /// update() 包装层。
    void updateInternal(double deltaSeconds);

    /// 手势起手帧的同步帧：刷新探针帧时钟 + 走一遍帧末哨兵，让手势数学
    /// 从一个「约束已满足」的位姿起步。dt=0 ⇒ 操控器 tick 全程空转，故它
    /// 等价于 beginFrame + resolveAtFrameEnd，与手势自身的状态重置无先后
    /// 依赖（这正是它能从操控器内部提到转发层的原因）。
    void syncFrameBeforeGesture();

    Camera* camera_;
    // 地形探针/突变滤波/碰撞钳位/groundState/位姿指纹全部归它。
    CameraConstraintSolver constraintSolver_;
    // 输入 → 位姿。持 camera_ 与 constraintSolver_ 的指针，不回指本类。
    GlobeGestureManipulator manipulator_;

    // 测量台冻结：true 时 update() 完全空转（见 setMeasurementFreeze）。
    bool measurementFreeze_ = false;

    // 测量台脚本化平移(见 setScriptedPan):active 时 update() 每帧原地偏航一步,
    // 内部帧计数确定性驱动,frames 帧后 hold。
    bool scriptedPanActive_ = false;
    int scriptedPanStartFrame_ = 0;  // 扫掠前先 hold 的帧数(让冷启动 settle)
    int scriptedPanFrames_ = 0;
    int scriptedPanFrame_ = 0;
    double scriptedPanYawPerFrameRad_ = 0.0;
};

} // namespace earth_engine
