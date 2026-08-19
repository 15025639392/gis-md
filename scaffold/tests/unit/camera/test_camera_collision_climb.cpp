// 低空 + 复杂地形:碰撞钳位的"单帧弹跳"回归测试(C-V6 观感缺口)。
//
// 背景(2026-08-19 排查):相机 600m AGL 朝 1500m 悬崖拖拽时,前瞻地板(探针
// 内环 0.15R 最大高)在距崖 180m 处一步抬到崖顶,clamp 沿 eye→anchor 线
// dolly,单帧 +950m/1779m 位移——"接近山体猛地弹开"。修复 = constrainEye 对
// 碰撞抬升加单事件上限(受控爬升),真穿地(低于脚下地形)仍立即抬出。
//
// 本测试用生产语义复现:拾取=生产 PickingService::pickTerrain(射线 vs 地形
// 高度场行进,命中可见面)、碰撞=探针区域采样、帧时钟=beginFrame 每步(缺帧
// 时钟会让探针只重建一次且半径取首帧 AGL=0,场景空转,见取证台伪影)。

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <cmath>
#include <optional>

#include "earth_engine/camera/CameraConstraintSolver.h"
#include "earth_engine/camera/controllers/FreeGlobeController.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/interaction/PickingService.h"
#include "earth_engine/scene/Camera.h"

using namespace earth_engine;

namespace {

constexpr int kW = 800;
constexpr int kH = 600;
constexpr double kLon0 = 106.5;   // 悬崖经度阈值:lon<kLon0 → 0m,≥ → 崖高
constexpr double kLat0 = 29.6;
constexpr double kCliffH = 1500.0;

double cliffHeightAt(const glm::dvec3& ecef) {
    const Cartographic c =
        Ellipsoid::WGS84().cartesianToCartographic(Vec3(ecef));
    return c.longitude() >= glm::radians(kLon0) ? kCliffH : 0.0;
}

double cliffHeightLngLat(double lonDeg, double latDeg) {
    return lonDeg >= kLon0 ? kCliffH : 0.0;
}

struct CliffHarness {
    Camera cam;
    CameraConstraintSolver solver;
    FreeGlobeController ctrl;

    CliffHarness() : ctrl(&cam, &solver) {
        cam.setPerspective(glm::radians(60.0), 1.0, 5.0e7);
        ctrl.setViewport(kW, kH);
        ctrl.setSurfacePicker(
            [this](float x, float y, Vec3& out) -> bool {
                PickingService svc;
                const PickResult r = svc.pickTerrain(
                    x, y, cam, double(kW), double(kH),
                    [](double lng, double lat) -> float {
                        return static_cast<float>(cliffHeightLngLat(
                            glm::degrees(lng), glm::degrees(lat)));
                    });
                if (!r.isValid()) return false;
                out = r.worldPosition;
                return true;
            });
        const auto& e = Ellipsoid::WGS84();
        solver.setTerrainHeightFunc(
            [](const Vec3& p) -> std::optional<double> {
                return cliffHeightAt(p.raw());
            });
        solver.setTerrainAreaSampleFunc(
            [this, &e](const Vec3& groundEcef, double radiusMeters,
                       const std::vector<glm::dvec2>& offsets,
                       std::vector<CameraConstraintSolver::TerrainSample>& out) {
                (void)radiusMeters;
                out.clear();
                const glm::dvec3 up =
                    e.geodeticSurfaceNormal(groundEcef).raw();
                glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
                const double el = glm::length(east);
                east = el > 1e-9 ? east / el : glm::dvec3(1, 0, 0);
                const glm::dvec3 north = glm::normalize(glm::cross(up, east));
                for (const glm::dvec2& off : offsets) {
                    const glm::dvec3 p =
                        groundEcef.raw() + east * off.x + north * off.y;
                    const Cartographic c =
                        e.cartesianToCartographic(Vec3(p));
                    const double h = cliffHeightLngLat(
                        glm::degrees(c.longitude()),
                        glm::degrees(c.latitude()));
                    CameraConstraintSolver::TerrainSample sm;
                    sm.valid = true;
                    sm.heightMeters = h;
                    sm.surfaceEcef = e.cartographicToCartesian(
                        Cartographic::fromRadians(c.longitude(),
                                                  c.latitude(), h));
                    out.push_back(sm);
                }
            });
        solver.setTerrainRevisionFunc([]() -> uint64_t { return 1; });
    }

    // 相机置于崖低侧 lon0−lonOffsetDeg 上空 aglMeters 处,朝东下俯 lookDownDeg。
    void placeCamera(double lonOffsetDeg, double aglMeters,
                     double lookDownDeg) {
        const auto& e = Ellipsoid::WGS84();
        const Vec3 ground = e.cartographicToCartesian(
            Cartographic::fromDegrees(kLon0 - lonOffsetDeg, kLat0, 0.0));
        const glm::dvec3 up = e.geodeticSurfaceNormal(ground).raw();
        const glm::dvec3 east =
            glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
        const glm::dvec3 eye = ground.raw() + up * aglMeters;
        const double pr = glm::radians(lookDownDeg);
        const glm::dvec3 dir =
            glm::normalize(east * std::cos(pr) - up * std::sin(pr));
        const glm::dvec3 camUp =
            glm::normalize(glm::cross(dir, glm::cross(up, dir)));
        cam.setView(Vec3(eye), Vec3(dir), Vec3(camUp));
    }

    double eyeAlt() const {
        return Ellipsoid::WGS84()
            .cartesianToCartographic(cam.position())
            .height();
    }

    // 相机相对起点的东向位移(米,经度差换算,纬度近似足够)。
    double eastMetersFrom(const Cartographic& start) const {
        const Cartographic c =
            Ellipsoid::WGS84().cartesianToCartographic(cam.position());
        const double dLon = c.longitude() - start.longitude();
        return glm::degrees(dLon) * 111320.0 *
               std::cos(start.latitude());
    }
};

}  // namespace

// 近地(NearGround,pitch≥60°)拖向 1500m 悬崖:碰撞抬升必须受控爬升,不得
// 单帧弹跳;全程不得穿地。
TEST(CollisionClimb, NearGroundCliffNoSingleFramePop) {
    CliffHarness h;
    // lookDown=30° ⇒ code pitch=60° ⇒ NearGround(契约 1.2)。
    h.placeCamera(0.001, 600.0, 30.0);

    const double startAlt = h.eyeAlt();
    h.ctrl.onDragStart(400.0f, 300.0f, 0.0);

    double maxFrameDAlt = 0.0;
    double maxEyeAlt = startAlt;
    double minAgl = 1e18;
    double prevAlt = startAlt;
    for (int i = 1; i <= 60; ++i) {
        h.solver.beginFrame();  // 生产路径 CameraSystem::update 每帧调用
        h.ctrl.onDragMove(400.0f, 300.0f + 10.0f * i, i * 0.016);
        const double alt = h.eyeAlt();
        maxFrameDAlt = std::max(maxFrameDAlt, std::abs(alt - prevAlt));
        maxEyeAlt = std::max(maxEyeAlt, alt);
        prevAlt = alt;
        const double terr = cliffHeightAt(h.cam.position().raw());
        minAgl = std::min(minAgl, alt - terr);
    }

    // 场景有效性:碰撞真的发火(相机被抬升了,不是空转)。
    EXPECT_GT(maxEyeAlt - startAlt, 50.0)
        << "场景失效:碰撞钳位没抬升相机,探针/帧时钟路径没走到";
    // 核心回归:单帧高度变化 ≤ 30m(修复前 950m)。上限=求解器单事件爬升
    // 上限 25m + 数值容差;若调大上限需同步改这里。
    EXPECT_LE(maxFrameDAlt, 30.0)
        << "相机被单帧弹起:前瞻地板仍是一次性抬满,限速没生效";
    // C-V6:不穿地(AGL 不得为负)。
    EXPECT_GT(minAgl, -1.0) << "相机穿模到地形下";
}

// 快速推进(Space 模式拖球,推进量大于爬升速度)跨过崖边:即使相机在爬升未完
// 成前越过崖边,穿地守卫也必须立即把相机抬出(允许残余跳变,不允许穿地)。
TEST(CollisionClimb, FastAdvancePenetrationGuardHolds) {
    CliffHarness h;
    // lookDown=45° ⇒ code pitch=45° ⇒ Space 拖球(推进更快,更容易越过崖边)。
    h.placeCamera(0.001, 600.0, 45.0);

    h.ctrl.onDragStart(400.0f, 300.0f, 0.0);
    double minAgl = 1e18;
    for (int i = 1; i <= 120; ++i) {
        h.solver.beginFrame();
        h.ctrl.onDragMove(400.0f, 300.0f + 10.0f * i, i * 0.016);
        const double alt = h.eyeAlt();
        const double terr = cliffHeightAt(h.cam.position().raw());
        minAgl = std::min(minAgl, alt - terr);
    }
    EXPECT_GT(minAgl, -1.0) << "快速推进跨崖时穿地,穿地守卫没兜住";
}

// 远处(≈480m)起手朝崖拖拽:锚点必须落在可见崖面(射线上、相机下方),拖拽
// 增益恢复——修复前锚点被钳到眼旁(50m),600px 只挪 24.7m(拖不动/P1)。
TEST(CollisionClimb, NearGroundFarCliffDragNotFrozen) {
    CliffHarness h;
    h.placeCamera(0.005, 600.0, 30.0);

    const Cartographic start =
        Ellipsoid::WGS84().cartesianToCartographic(h.cam.position());
    const double startAlt = h.eyeAlt();
    h.ctrl.onDragStart(400.0f, 300.0f, 0.0);

    double maxFrameDAlt = 0.0;
    double minAgl = 1e18;
    double prevAlt = startAlt;
    for (int i = 1; i <= 80; ++i) {
        h.solver.beginFrame();
        h.ctrl.onDragMove(400.0f, 300.0f + 8.0f * i, i * 0.016);
        const double alt = h.eyeAlt();
        maxFrameDAlt = std::max(maxFrameDAlt, std::abs(alt - prevAlt));
        prevAlt = alt;
        const double terr = cliffHeightAt(h.cam.position().raw());
        minAgl = std::min(minAgl, alt - terr);
    }

    const double dEast = h.eastMetersFrom(start);
    // 修复前:增益崩塌,相机基本不动(dEast≈24.7m、eyeAlt 恒 600)。
    EXPECT_GT(dEast, 150.0) << "相机仍被冻住:锚点贴眼/增益崩塌(P1)未修复";
    EXPECT_LE(maxFrameDAlt, 30.0) << "单帧弹跳回潮";
    EXPECT_GT(minAgl, -1.0) << "相机穿模";
}
