#include "CameraConstraintSolver.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace earth_engine {

namespace {

// Keep a small visual floor to avoid clipping the ellipsoid surface near
// ground. 数值由头文件的净空↔near 耦合契约持有(static_assert 锁定)。
constexpr double kMinAltitudeMeters =
    CameraConstraintSolver::kMinClearanceMeters;
// 非对称地形突变滤波(仅数据驱动的样本变化走滤波,用户驱动一律立即):
// 上升立即(刚体优先——新瓦片证明脚下是山,延迟=穿模 Cesium 对称 10% 规则的
// 缺陷);|Δ| ≤ max(Abs, Rel·h) 的小变动立即(LOD 抖动,绝对项防海面 h≈0 时
// 相对判据退化);大幅下降按 τ 指数逼近(dt 感知,掉帧时收敛速率不变)。
constexpr double kTerrainFilterAbsStepMeters = 10.0;
constexpr double kTerrainFilterRelStep = 0.1;
constexpr double kTerrainFilterDecayTauSeconds = 0.5;
// 近场探针几何:中心点 + 同心三环×8 方位(旋转对称——故意不做视线前向偏置:
// 偏置只能放宽 near 不增安全,还让原地旋转时 near 抖动)。碰撞口径=内环
// (r≤0.15R)+扫掠走廊最大高;near 口径=全部有效采样点的三维最小距离。
// R = clamp(max(2·AGL, 0.6·单帧水平位移), 200m, 20km)——位移项把静态点检
// 升级成廉价扫掠检测,单帧跨山脊不再隧穿;超出 20km 的远场由"盘外墙"下界
// 项接管(见动态 near 公式)。
constexpr double kProbeRingFractions[] = {0.15, 0.40, 1.0};
constexpr int kProbeRingAzimuths = 8;
constexpr double kProbeMinRadiusMeters = 200.0;
constexpr double kProbeMaxRadiusMeters = 20000.0;
constexpr double kProbeAglFactor = 2.0;
constexpr double kProbeSweptFactor = 0.6;
constexpr double kProbeCollisionFraction = 0.15;
// 中心漂移超过内环半径的 1/4 (=0.25×0.15×R) 即重建(每帧至多 1 次)。
constexpr double kProbeDriftRebuildFraction = 0.0375;
// 锚点退出方向的最小竖直增益:|dot(unit(eye−anchor), n̂)| 低于此值时后退
// 换不来高度,退回径向抬升(该位姿下锚点已在掠射病态区,保锚判据不适用)。
constexpr double kAnchorExitMinVerticalGain = 0.2;

} // namespace

void CameraConstraintSolver::setTerrainHeightFunc(TerrainHeightFunc func) {
    terrainHeightFunc_ = std::move(func);
}

void CameraConstraintSolver::setTerrainAreaSampleFunc(
    TerrainAreaSampleFunc func) {
    terrainAreaSampleFunc_ = std::move(func);
    terrainProbe_.valid = false;
}

void CameraConstraintSolver::setTerrainRevisionFunc(
    std::function<uint64_t()> func) {
    terrainRevisionFunc_ = std::move(func);
}

void CameraConstraintSolver::commitPose(const glm::dvec3& eye) {
    lastResolvedEye_ = eye;
    hasLastResolvedEye_ = true;
}

glm::dvec3 CameraConstraintSolver::constrainEye(
    const glm::dvec3& eye,
    bool userDriven,
    double deltaSeconds,
    const glm::dvec3* pinnedAnchorWorld) {
    if (glm::length(eye) < 1e-6) return eye;

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 eyeVec(eye);
    const Cartographic cart = ellipsoid.cartesianToCartographic(eyeVec);

    // Fast path: already above the tallest possible terrain + floor, so the
    // clamp below can never change the eye. Skip the costly terrain query.
    if (cart.height() >= kMaxTerrainHeightMeters + kMinAltitudeMeters) {
        groundState_.valid = true;
        groundState_.hasTerrainData = false;
        groundState_.terrainHeightMeters = filteredTerrainHeight_;
        groundState_.heightAboveTerrain = cart.height() - filteredTerrainHeight_;
        // 全球地形不超过 9000m ⇒ 最近几何至少在这个竖直距离之外。
        groundState_.nearestGeometryMeters =
            cart.height() - kMaxTerrainHeightMeters;
        return eye;
    }

    const Vec3 surface = ellipsoid.projectToSurface(eyeVec);
    const Vec3 normal = ellipsoid.geodeticSurfaceNormal(surface);

    // 无地形数据(未加载 / 无覆盖瓦片)时——不能当海平面 0 处理,否则相机会被
    // 允许下沉到未加载山体表面之下,瓦片加载后又被顶回(dip→pop)。保守回退
    // 到滤波现值(初值 0)。有数据时经非对称突变滤波更新。探针注入后单点
    // TerrainHeightFunc 退为回退路径(既有测试/未接探针的宿主继续可用)。
    bool sampledThisCall = false;
    double rawHeight = 0.0;
    if (terrainAreaSampleFunc_) {
        refreshTerrainProbeIfNeeded(eye, surface);
        if (terrainProbe_.valid && terrainProbe_.hasData) {
            rawHeight = terrainProbe_.collisionMaxHeight;
            sampledThisCall = true;
        }
    } else if (terrainHeightFunc_) {
        const std::optional<double> sampled = terrainHeightFunc_(surface);
        if (sampled) {
            rawHeight = *sampled;
            sampledThisCall = true;
        }
    }
    if (sampledThisCall) {
        updateFilteredTerrainHeight(rawHeight, userDriven, deltaSeconds);
    }
    groundState_.valid = true;
    groundState_.hasTerrainData = sampledThisCall;
    groundState_.terrainHeightMeters = filteredTerrainHeight_;
    groundState_.heightAboveTerrain = cart.height() - filteredTerrainHeight_;
    // 最近几何距离(动态 near 口径):探针采样点三维最小距离 ∧ 盘外墙下界。
    // 无探针(单点回退/无数据)时退化为竖直 AGL——无侧向信息,与旧公式同构
    // 但扣掉了地形高(旧公式按椭球 nadir 算,高原上空 near 被拉大 5-10 倍,
    // 是"看到山内部"的根因)。
    if (terrainAreaSampleFunc_ && terrainProbe_.valid &&
        !terrainProbe_.samplePointsEcef.empty()) {
        double dMin = std::numeric_limits<double>::infinity();
        for (const glm::dvec3& p : terrainProbe_.samplePointsEcef) {
            dMin = std::min(dMin, glm::length(eye - p));
        }
        const double wall = std::sqrt(
            terrainProbe_.radiusMeters * terrainProbe_.radiusMeters +
            std::pow(std::max(0.0, cart.height() - kMaxTerrainHeightMeters),
                     2.0));
        groundState_.nearestGeometryMeters = std::min(dMin, wall);
    } else {
        groundState_.nearestGeometryMeters =
            std::max(cart.height() - std::max(filteredTerrainHeight_, 0.0),
                     1.0);
    }

    const double minHeight = std::max(filteredTerrainHeight_, 0.0) +
                             kMinAltitudeMeters;

    if (cart.height() >= minHeight) return eye;
    groundState_.heightAboveTerrain = minHeight - filteredTerrainHeight_;

    // 退出方向:有锚点时沿 eye→anchor 直线反向 dolly——eye→anchor 方向与
    // dir/up 全不变 ⇒ 锚点像素严格不动(径向抬升会泄漏 anchorErr,是旧实现
    // 唯一的保锚漏洞)。大地高沿该直线非线性,牛顿式迭代三轮收敛到厘米级。
    // 直线近水平(后退换不来高度)时退回径向抬升——该位姿下锚点已在掠射
    // 病态区,pin 走转台混合,保锚判据本就不适用。
    if (pinnedAnchorWorld) {
        const glm::dvec3 away = eye - *pinnedAnchorWorld;
        const double awayLen = glm::length(away);
        if (awayLen > 1e-6) {
            const glm::dvec3 u = away / awayLen;
            double gain = glm::dot(u, normal.raw());
            if (gain >= kAnchorExitMinVerticalGain) {
                glm::dvec3 candidate = eye;
                for (int i = 0; i < 3; ++i) {
                    const Cartographic c =
                        ellipsoid.cartesianToCartographic(Vec3(candidate));
                    const double deficit = minHeight - c.height();
                    if (deficit <= 1e-3) break;
                    const glm::dvec3 n =
                        ellipsoid.geodeticSurfaceNormal(
                            ellipsoid.projectToSurface(Vec3(candidate))).raw();
                    gain = glm::dot(u, n);
                    if (gain < kAnchorExitMinVerticalGain) break;
                    candidate += u * (deficit / gain);
                }
                if (ellipsoid.cartesianToCartographic(Vec3(candidate))
                        .height() >= minHeight - 1e-2) {
                    return candidate;
                }
            }
        }
    }
    return (surface + normal * minHeight).raw();
}

void CameraConstraintSolver::refreshTerrainProbeIfNeeded(const glm::dvec3& eye,
                                                         const Vec3& surface) {
    const uint64_t revision =
        terrainRevisionFunc_ ? terrainRevisionFunc_() : 0;
    const double aglPrev = std::max(0.0, groundState_.heightAboveTerrain);
    // 单帧水平位移(扫掠项):与上次解算位置的水平分量差。
    glm::dvec3 sweepVec(0.0);
    if (hasLastResolvedEye_) {
        const glm::dvec3 up = glm::normalize(eye);
        const glm::dvec3 d = eye - lastResolvedEye_;
        sweepVec = d - up * glm::dot(d, up);
    }
    const double sweep = glm::length(sweepVec);
    const double radius = std::clamp(
        std::max(kProbeAglFactor * aglPrev, kProbeSweptFactor * sweep),
        kProbeMinRadiusMeters, kProbeMaxRadiusMeters);

    const bool invalidated =
        !terrainProbe_.valid || revision != terrainProbe_.revision ||
        std::abs(radius - terrainProbe_.radiusMeters) >
            0.25 * terrainProbe_.radiusMeters ||
        glm::length(surface.raw() - terrainProbe_.centerSurfaceEcef) >
            kProbeDriftRebuildFraction * terrainProbe_.radiusMeters;
    if (!invalidated || lastProbeRebuildFrame_ == frameIndex_) {
        return;
    }
    lastProbeRebuildFrame_ = frameIndex_;

    // 本地 ENU 基(极点退化回退 ECEF X 轴,与 headingFromFrame 同法)。
    const glm::dvec3 up =
        Ellipsoid::WGS84().geodeticSurfaceNormal(surface).raw();
    glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), up);
    const double eastLen = glm::length(east);
    east = eastLen > 1e-9 ? east / eastLen : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 north = glm::cross(up, east);

    std::vector<glm::dvec2> offsets;
    std::vector<char> collisionScope;
    offsets.reserve(2 + kProbeRingAzimuths * 3 + 4);
    offsets.push_back(glm::dvec2(0.0, 0.0));
    collisionScope.push_back(1);
    constexpr int kRingCount =
        static_cast<int>(sizeof(kProbeRingFractions) /
                         sizeof(kProbeRingFractions[0]));
    for (int ri = 0; ri < kRingCount; ++ri) {
        const double r = kProbeRingFractions[ri] * radius;
        for (int a = 0; a < kProbeRingAzimuths; ++a) {
            // 环间错开半个方位步长,方位覆盖更均匀。
            const double phi = (static_cast<double>(a) + 0.5 * ri) *
                (2.0 * glm::pi<double>() / kProbeRingAzimuths);
            offsets.push_back(
                glm::dvec2(r * std::cos(phi), r * std::sin(phi)));
            collisionScope.push_back(ri == 0 ? 1 : 0);
        }
    }
    // 扫掠走廊:朝上次位置方向补采样,盖住单帧跨越的山脊(环是离散圆,
    // 山脊落在环间会被漏掉;走廊沿位移线段密采)。
    if (sweep > kProbeCollisionFraction * radius) {
        const glm::dvec2 back(-glm::dot(sweepVec, east),
                              -glm::dot(sweepVec, north));
        for (double f : {0.25, 0.5, 0.75, 1.0}) {
            offsets.push_back(back * f);
            collisionScope.push_back(1);
        }
    }

    std::vector<TerrainSample> samples;
    terrainAreaSampleFunc_(surface, radius, offsets, samples);

    terrainProbe_.valid = true;
    terrainProbe_.revision = revision;
    terrainProbe_.centerSurfaceEcef = surface.raw();
    terrainProbe_.radiusMeters = radius;
    terrainProbe_.hasData = false;
    terrainProbe_.samplePointsEcef.clear();
    double collisionMax = -std::numeric_limits<double>::infinity();
    double anyMax = -std::numeric_limits<double>::infinity();
    const size_t n = std::min(samples.size(), offsets.size());
    for (size_t i = 0; i < n; ++i) {
        if (!samples[i].valid) continue;
        terrainProbe_.hasData = true;
        terrainProbe_.samplePointsEcef.push_back(samples[i].surfaceEcef.raw());
        anyMax = std::max(anyMax, samples[i].heightMeters);
        if (collisionScope[i]) {
            collisionMax = std::max(collisionMax, samples[i].heightMeters);
        }
    }
    // 碰撞口径内无有效样本而外环有(局部数据洞):保守取全部样本最大高。
    terrainProbe_.collisionMaxHeight =
        std::isfinite(collisionMax) ? collisionMax
        : (std::isfinite(anyMax) ? anyMax : 0.0);
}

void CameraConstraintSolver::updateFilteredTerrainHeight(double rawHeightMeters,
                                                         bool userDriven,
                                                         double deltaSeconds) {
    const double delta = rawHeightMeters - filteredTerrainHeight_;
    // 非对称：用户驱动一律立即；数据驱动的上升立即（刚体优先——新瓦片证明
    // 脚下是山，延迟生效 = 穿模）；小变动立即（吸收 LOD 抖动本身没有意义，
    // 绝对项防 h≈0 海面时相对判据退化）；只有大幅下降才指数逼近——它不影响
    // 相机（钳位只抬不压），平滑的是下游消费者（动态 near）看到的地面。
    if (userDriven || delta > 0.0 ||
        std::abs(delta) <=
            std::max(kTerrainFilterAbsStepMeters,
                     kTerrainFilterRelStep * std::abs(filteredTerrainHeight_))) {
        filteredTerrainHeight_ = rawHeightMeters;
    } else {
        filteredTerrainHeight_ +=
            delta * (1.0 - std::exp(-deltaSeconds /
                                    kTerrainFilterDecayTauSeconds));
    }
}

} // namespace earth_engine
