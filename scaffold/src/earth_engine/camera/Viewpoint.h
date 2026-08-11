#pragma once

#include "../core/geodesy/Cartographic.h"
#include "../core/math/Vec3.h"

#include <glm/glm.hpp>
#include <functional>
#include <optional>

namespace earth_engine {

/// 参考系。两个 provider 都为空 = 世界系(ECEF + 原点处 ENU)。
///
/// **为什么用 provider 回调而不是 4×4 矩阵**:表达力等价,但矩阵形式会诱使
/// `Camera` 持有它(Cesium `camera.transform` 那条路),从而让全局代码陷入
/// local/WC 二义性——cesium-native 的 `adjustHeightForTerrain` 里就有一段丑陋的
/// save/restore 来回切。provider 形式把参考系关在控制器内部,`Camera` 只见世界
/// 位姿,solver / 渲染 / 拾取全部无感。
struct ViewpointFrame {
    /// 参考系原点。空 = 世界系。返回 false = 目标暂不可用(载体还没生成/已销毁),
    /// 调用方应保持上一帧,**不要**回落到世界系——那会是一次瞬移。
    std::function<bool(glm::dvec3& outOriginEcef)> originProvider;

    /// 参考系姿态(三列 = 右/前/上)。空 = 用原点处的 ENU。
    /// 提供时 = 完全固连载体机体系(roll 跟随载体)。
    std::function<bool(glm::dmat3& outFrame)> orientationProvider;

    bool isWorld() const { return !originProvider; }
};

/// 接口层的视角表述。**每个字段 optional ⇒「部分 viewpoint」语义**:只写想改的,
/// 其余保持当前。这是全设计里性价比最高的一处(抄 osgEarth `Viewpoint`)。
///
/// | 需求 | 表达 |
/// |---|---|
/// | 复位正北 | `{.headingRadians = 0}` |
/// | 只改俯仰 | `{.pitchRadians = -M_PI/4}` |
/// | 俯视某点 | `{.targetGeo=g, .headingRadians=0, .pitchRadians=-M_PI/2, .rangeMeters=h}` |
/// | 跟踪载体保持北上 | `{.frame={.originProvider=f}, .rangeMeters=500}` |
///
/// ⚠️ **heading/pitch/roll 定义在「参考系原点」的局部系里**,原点按
/// tether 原点 → `eyeGeo` → `targetGeo` → 当前 eye 四级回退(见
/// `CameraSystem::setViewpoint`)。这一条必须记住:同一组 hpr 配不同的原点是不同
/// 的位姿,因为 ENU 基底在球面上逐点转动。
struct Viewpoint {
    ViewpointFrame frame;  ///< 缺省 = 世界系

    // ---- 位置:eyeGeo 与 (targetGeo + rangeMeters) 二选一 ----
    /// 直接给相机位置。与 `targetGeo` 是**二选一**;真同时给出时 **eyeGeo 胜**——
    /// 它是 `currentViewpoint()` 的规范输出形式,这条 tie-break 正是往返恒等的前提。
    std::optional<Cartographic> eyeGeo;
    /// 焦点。相机置于焦点外 rangeMeters 处并看向它。
    std::optional<Cartographic> targetGeo;
    /// 到焦点/参考系原点的距离。有 `targetGeo` 而缺它 ⇒ 保持当前距离。
    /// ⚠️ **没有焦点时(纯朝向写入 / 用 `eyeGeo` 直接给了相机位置)本字段被忽略**:
    /// 它描述的是到焦点的距离,无焦点时无意义。`currentViewpoint()` 会顺带报出
    /// "视线到椭球命中点的距离",照用它就成了"沿视线后退这么多",往返即失恒等。
    std::optional<double> rangeMeters;

    // ---- 朝向(在参考系内):缺省 = 保持当前 ----
    std::optional<double> headingRadians;  ///< 0 = 正北,顺时针为正
    std::optional<double> pitchRadians;    ///< 0 = 水平,-π/2 = 正俯视
    std::optional<double> rollRadians;     ///< 绕视线轴
};

} // namespace earth_engine
