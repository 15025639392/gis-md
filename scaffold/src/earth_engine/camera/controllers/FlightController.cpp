#include "FlightController.h"

#include "../CameraConstraintSolver.h"
#include "../../core/geodesy/Cartographic.h"
#include "../../core/geodesy/Ellipsoid.h"
#include "../../core/math/Vec3.h"
#include "../../scene/Camera.h"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace earth_engine {

namespace {

// 时长启发式(抄 cesium `CameraFlightPath.js`):2~3 秒。
constexpr double kFlightBaseSeconds = 2.0;
constexpr double kFlightMaxSeconds = 3.0;
constexpr double kFlightDistancePerSecond = 1.0e6;

// 起终点重合判定(米)。低于它没有可飞的路径,直接落位。
constexpr double kDegenerateDistanceMeters = 1.0;

// 路径地形采样:沿曲线的采样点数,以及每点周围的方位数。
// 只沿路径取点会漏掉两点之间的山脊(和碰撞探针"环是离散圆、山脊落在环间会被
// 漏掉"是同一个几何事实),故每点再按半个采样间距取一圈。
constexpr int kPathSampleCount = 64;
constexpr int kPathSampleAzimuths = 4;

// 拱高形状 sin(πt) 在两端趋 0,低于此值不反解(否则除零爆炸)。端点自身的净空
// 是起终点位姿的问题,路径规划抬不动。
constexpr double kArchShapeFloor = 1e-3;

// 规划目标 = 地形 + 净空 × 本系数。**必须 > 1**:碰撞钳位在 1× 处触发,规划瞄准
// 1× 就是零余量,而路径采样与碰撞探针的采样几何本就不同(后者是相机周围的扫掠
// 走廊),读数差一点就把钳位逼出来。
constexpr double kArchClearanceSafetyFactor = 2.0;

// 缓动 QUINTIC_IN_OUT(cesium 默认)。
double quinticInOut(double t) {
    if (t < 0.5) {
        return 16.0 * t * t * t * t * t;
    }
    const double f = -2.0 * t + 2.0;
    return 1.0 - (f * f * f * f * f) / 2.0;
}

/// 把 delta 归一到 (−π, π] —— heading 走最短弧。不 unwrap 的话「从 350° 转到
/// 10°」会绕 340° 的远路,画面上就是转了一整圈。
double shortestAngleDelta(double from, double to) {
    double d = to - from;
    const double twoPi = 2.0 * glm::pi<double>();
    while (d > glm::pi<double>()) d -= twoPi;
    while (d <= -glm::pi<double>()) d += twoPi;
    return d;
}

}  // namespace

FlightController::FlightController(Camera* camera,
                                   CameraConstraintSolver* solver)
    : camera_(camera), solver_(solver) {}

bool FlightController::start(const CameraPose& from,
                             const CameraPose& to,
                             double durationSecondsOverride) {
    cancel();

    const double distance = glm::length(to.eye - from.eye);
    const auto& ellipsoid = Ellipsoid::WGS84();

    std::optional<SimplePlanarEllipsoidCurve> curve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            ellipsoid, Vec3(from.eye), Vec3(to.eye));
    // 起终点重合或曲线退化(共线过地心):没有可飞的路径。返回 false 让调用方
    // 直接落位,而不是进入一个每帧原地不动、还要等 duration 才结束的"飞行"。
    if (distance < kDegenerateDistanceMeters || !curve) {
        return false;
    }

    from_ = from;
    to_ = to;
    curve_ = std::move(curve);
    archHeightMeters_ = planArchHeight(from.eye, to.eye, *curve_);

    durationSeconds_ = durationSecondsOverride > 0.0
        ? durationSecondsOverride
        : std::min(std::ceil(distance / kFlightDistancePerSecond) +
                       kFlightBaseSeconds,
                   kFlightMaxSeconds);

    double unusedRange = 0.0;
    from_.toFrame(from.eye, CameraPose::enuFrameAt(from.eye), fromHeading_,
                  fromPitch_, fromRoll_, unusedRange);
    to_.toFrame(to.eye, CameraPose::enuFrameAt(to.eye), toHeading_, toPitch_,
                toRoll_, unusedRange);

    elapsedSeconds_ = 0.0;
    progress_ = 0.0;
    completed_ = false;
    active_ = true;
    return true;
}

void FlightController::cancel() {
    active_ = false;
    completed_ = false;
    elapsedSeconds_ = 0.0;
    progress_ = 0.0;
    curve_.reset();
}

bool FlightController::consumeCompleted() {
    const bool was = completed_;
    completed_ = false;
    return was;
}

void FlightController::tick(double deltaSeconds) {
    if (!active_ || !curve_) {
        return;
    }
    // dt<=0(同帧多次 update / 手势起手的同步帧)不推进时间,但也不该被当成结束。
    if (deltaSeconds > 0.0) {
        elapsedSeconds_ += deltaSeconds;
    }
    progress_ = durationSeconds_ > 0.0
        ? std::clamp(elapsedSeconds_ / durationSeconds_, 0.0, 1.0)
        : 1.0;

    if (progress_ >= 1.0) {
        // **精确落到终点位姿**,不用插值的收敛值:验收判据是终点相对误差 < 1e-3,
        // 靠缓动自然收敛会留下与实现相关的残差。
        camera_->setView(Vec3(to_.eye), Vec3(to_.direction), Vec3(to_.up));
        active_ = false;
        completed_ = true;
        curve_.reset();
        return;
    }

    const double eased = quinticInOut(progress_);
    // 拱高按 sin(πt) 起落:两端为 0(严格过起终点),中段最高。
    const double arch = archHeightMeters_ * std::sin(glm::pi<double>() * eased);
    const glm::dvec3 eye = curve_->getPosition(eased, arch).raw();

    const double heading =
        fromHeading_ + shortestAngleDelta(fromHeading_, toHeading_) * eased;
    const double pitch = fromPitch_ + (toPitch_ - fromPitch_) * eased;
    const double roll =
        fromRoll_ + shortestAngleDelta(fromRoll_, toRoll_) * eased;

    // 朝向按**当前位置**的 ENU 解释——飞行途中位置在动,ENU 基底随之转动,
    // 用起点的基底会让"保持正北"在长途飞行里逐渐歪掉。
    const CameraPose pose = CameraPose::fromFrame(
        eye, CameraPose::enuFrameAt(eye), heading, pitch, roll, 0.0);
    camera_->setView(Vec3(pose.eye), Vec3(pose.direction), Vec3(pose.up));
}

double FlightController::planArchHeight(
    const glm::dvec3& fromEcef,
    const glm::dvec3& toEcef,
    const SimplePlanarEllipsoidCurve& curve) const {
    const auto& ellipsoid = Ellipsoid::WGS84();

    // 拱高的形状是 A·sin(πt)(两端为 0 故严格过起终点)。**必须逐点反解 A**:
    // 只在中点算一次是错的——山脊落在 t=0.3 时那里的拱只有 A·sin(0.3π)=0.81A,
    // 中点抬够不等于山脊处抬够。第一版就是这么写的,机制判据(飞行期钳位次数
    // 必须为 0)当场把它抓出来了。
    double arch = 0.0;

    // ① 看见两端所需高度 = 两端之间地球的"鼓包"(弦到弧的矢高)。低于它时目的地
    //    在地平线之下,飞行全程看不到落点。它在中点处要求最高,故按中点反解。
    const double fromLength = glm::length(fromEcef);
    const double toLength = glm::length(toEcef);
    if (fromLength > 1.0 && toLength > 1.0) {
        const double cosAngle = std::clamp(
            glm::dot(fromEcef / fromLength, toEcef / toLength), -1.0, 1.0);
        const double halfAngle = std::acos(cosAngle) * 0.5;
        const double radius = std::min(fromLength, toLength);
        const double seeBothEnds = radius * (1.0 - std::cos(halfAngle));
        const double baseAtMid =
            ellipsoid.cartesianToCartographic(curve.getPosition(0.5, 0.0))
                .height();
        arch = std::max(arch, seeBothEnds - baseAtMid);
    }

    // ② 路径地形。没有注入采样口(host 测试/无地形)时整项跳过——那时不该凭空
    //    抬高,否则短途飞行会莫名其妙先窜上天。
    const auto& areaSample = solver_->terrainAreaSampleFunc();
    const auto& heightAt = solver_->terrainHeightFunc();
    if (!areaSample && !heightAt) {
        return std::max(0.0, arch);
    }

    // 相邻采样点的间距,用作每点的环半径:山脊落在两点之间时仍被环扫到
    // (和碰撞探针"环是离散圆、山脊落在环间会被漏掉"是同一个几何事实)。
    const double pathLength = glm::length(toEcef - fromEcef);
    const double ringRadius =
        std::max(1.0, pathLength / static_cast<double>(kPathSampleCount));

    std::vector<glm::dvec2> offsets;
    offsets.reserve(1 + kPathSampleAzimuths);
    offsets.emplace_back(0.0, 0.0);
    for (int a = 0; a < kPathSampleAzimuths; ++a) {
        const double theta = 2.0 * glm::pi<double>() * static_cast<double>(a) /
                             static_cast<double>(kPathSampleAzimuths);
        offsets.emplace_back(ringRadius * std::cos(theta),
                             ringRadius * std::sin(theta));
    }

    std::vector<CameraConstraintSolver::TerrainSample> out;
    for (int i = 0; i <= kPathSampleCount; ++i) {
        const double t =
            static_cast<double>(i) / static_cast<double>(kPathSampleCount);
        const double shape = std::sin(glm::pi<double>() * t);
        // 两端 shape→0:拱高在那里无论多大都抬不动。端点自身的净空是起终点
        // 位姿的问题(由帧末哨兵负责),不是路径规划能解的,跳过避免除零爆炸。
        if (shape <= kArchShapeFloor) {
            continue;
        }

        const Vec3 pathPoint = curve.getPosition(t, 0.0);
        const double baseHeight =
            ellipsoid.cartesianToCartographic(pathPoint).height();
        // 采样点要落在地表:路径点本身在空中,把它投到椭球面再采。
        Cartographic geo = ellipsoid.cartesianToCartographic(pathPoint);
        geo.setHeight(0.0);
        const Vec3 groundPoint = ellipsoid.cartographicToCartesian(geo);

        double terrainHere = 0.0;
        if (areaSample) {
            out.clear();
            areaSample(groundPoint, ringRadius, offsets, out);
            for (const auto& sample : out) {
                if (sample.valid) {
                    terrainHere = std::max(terrainHere, sample.heightMeters);
                }
            }
        } else {
            const std::optional<double> h = heightAt(groundPoint);
            if (h) {
                terrainHere = *h;
            }
        }

        // 规划目标高于**执行阈值**:碰撞钳位在 AGL < kMinClearanceMeters 时触发,
        // 规划若恰好瞄准 1× 就是零余量——路径采样与碰撞探针的采样几何不同
        // (后者是相机周围的扫掠走廊),读数差一点点就会把钳位逼出来。留 2× 让
        // "钳位结构性不触发"成为真的结构性,而不是靠运气压线。
        const double target =
            terrainHere + CameraConstraintSolver::kMinClearanceMeters *
                              kArchClearanceSafetyFactor;
        arch = std::max(arch, (target - baseHeight) / shape);
    }

    return std::max(0.0, arch);
}

} // namespace earth_engine
