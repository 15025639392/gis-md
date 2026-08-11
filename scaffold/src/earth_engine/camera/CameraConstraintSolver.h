#pragma once

#include "../core/math/Vec3.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace earth_engine {

/// 相机位姿的地形约束求解器：近场探针、突变滤波、碰撞钳位、地面状态快照。
///
/// 纯策略执行者——不知道手势、惯性、飞行，只回答一个问题：「给定 eye，
/// 合法的 eye 是什么」。**唯一调用者是编排层的约束出口**（当前
/// CameraSystem::resolveConstraints）；任何其他调用点都是在绕过单一
/// 出口，那正是这次拆分要根治的形态。
class CameraConstraintSolver {
public:
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

    /// 只读取出注入的采样口。给**规划期**用(飞行路径沿线采地形抬拱高),不是给
    /// 逐帧解算用——逐帧路径一律走 `constrainEye`,别绕过它自己采样。
    /// 未注入时返回空 function,调用方须判空。
    const TerrainAreaSampleFunc& terrainAreaSampleFunc() const {
        return terrainAreaSampleFunc_;
    }
    const TerrainHeightFunc& terrainHeightFunc() const {
        return terrainHeightFunc_;
    }

    /// 相机相对地形的一次解算快照，每次 constrainEye 刷新。纯读，
    /// 供渲染层（动态 near）、测试与诊断消费，不含策略。
    struct GroundState {
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
    const GroundState& groundState() const { return groundState_; }

    /// 滤波后的近场地形高（米）。俯仰守卫用它做廉价净空预判，不重采样。
    double filteredTerrainHeight() const { return filteredTerrainHeight_; }

    /// 碰撞净空 ↔ 动态 near 的耦合契约（禁止单独改动其一）：净空保证
    /// "最近地形几何 ≥ kMinClearanceMeters"，near = Ratio×最近几何 ≥ Floor
    /// 才能既压住 z_ndc 病态区又不切脚下地面。
    static constexpr double kMinClearanceMeters = 50.0;
    static constexpr double kNearFloorMeters = 5.0;
    static constexpr double kNearSafetyRatio = 0.5;
    static_assert(kNearSafetyRatio * kMinClearanceMeters >= kNearFloorMeters,
                  "near 下限超过净空×安全比:近平面会切进脚下地面");

    /// Real-world terrain never exceeds ~8849 m (Everest); use a margin above it.
    /// When the eye is already higher than this plus the altitude floor, the terrain
    /// clamp can never trigger, so the (expensive, per-frame) terrain height query is
    /// pure waste — skip it. This is what keeps panning/zooming smooth at altitude:
    /// the query scans every loaded tile's mesh triangles (each with ECEF→geodetic
    /// round-trips) and otherwise costs >150 ms/frame.
    static constexpr double kMaxTerrainHeightMeters = 9000.0;

    /// 相机地心距上限（地球半径单位）。与净空一样属于**相机包络**约束，只是
    /// 方向朝外；放在这里是因为拆分后两侧都要读它（手势侧的 dolly/滑行封顶、
    /// 编排侧的 viewDistance 钳位），各写一份字面量就是典型的"同一事实两处
    /// 各推"。⚠️ 目前它只是调用点的一道闸，尚未进 constrainEye 的统一出口
    /// （因而无测试覆盖），并入出口是后续的事。
    static constexpr double kMaxDistanceEarthRadii = 30.0;

    /// 探针"每帧至多重建一次"的帧时钟推进。每帧恰好调一次（含手势事件内的
    /// update(0.0)——高频手势事件因此共享同帧探针）。
    void beginFrame() { ++frameIndex_; }

    /// 地形碰撞解算：把 eye 钳到滤波后地形高 + 视觉下限之上（仅抬升，不下压），
    /// 途中刷新探针/滤波与 groundState。
    /// @param pinnedAnchorWorld 非空时退出方向沿 eye→anchor 直线（方向与
    ///        dir/up 全不变 ⇒ 锚点像素严格不动）；该直线近水平（后退换不来
    ///        高度）时退回大地法线径向抬升。
    glm::dvec3 constrainEye(const glm::dvec3& eye,
                            bool userDriven,
                            double deltaSeconds,
                            const glm::dvec3* pinnedAnchorWorld);

    /// 记录本次解算落定的位姿。一次提交同时服务两件事：
    ///   ① eye = 下次扫掠走廊的位移基准；
    ///   ② (eye, dir) = 位姿指纹，供帧末哨兵判「期间有没有人绕过钳位裸写」。
    /// 两者本就同源同时机（都是"上次解算完相机在哪"），分开存只会漂。
    ///
    /// ⚠️ 必须在解算落定后调用，不能塞进 constrainEye 内部自动更新：
    /// constrainEye 开头才读上一次的基准算 sweep，塞进去就变成读自己。
    void commitPose(const glm::dvec3& eye, const glm::dvec3& direction);

    /// 位姿是否与上次 commitPose 记录的不同（阈值与历史实现一致：位置 1e-6 m、
    /// 方向 1e-9）。从未提交过时返回 false（没有基准可比，不能算"被改过"）。
    bool poseChangedSince(const glm::dvec3& eye,
                          const glm::dvec3& direction) const;

private:
    /// 近场探针按需重建（中心漂移/半径变化/代次变化；每帧至多 1 次）。
    void refreshTerrainProbeIfNeeded(const glm::dvec3& eye,
                                     const Vec3& surface);
    /// 非对称地形突变滤波：用户驱动/上升/小变动立即，数据驱动大幅下降
    /// 按 τ 指数逼近（见 .cpp 常量说明）。
    void updateFilteredTerrainHeight(double rawHeightMeters,
                                     bool userDriven,
                                     double deltaSeconds);

    TerrainHeightFunc terrainHeightFunc_;
    // 滤波后的近场地形高(米)。无数据时保持现值(保守回退,防 dip→pop)；
    // 数据驱动的突变经非对称滤波(updateFilteredTerrainHeight)。
    double filteredTerrainHeight_ = 0.0;
    GroundState groundState_;

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

    // 上次解算落定的位姿：eye 作扫掠基准，(eye, dir) 作帧末指纹。见 commitPose。
    bool hasLastResolvedPose_ = false;
    glm::dvec3 lastResolvedEye_{0.0};
    glm::dvec3 lastResolvedDirection_{0.0};
};

} // namespace earth_engine
