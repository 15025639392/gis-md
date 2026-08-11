#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include "earth_engine/camera/CameraSystem.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"

using namespace earth_engine;

namespace {

constexpr double kEarthRadiusMeters = 6378137.0;

// 平地假体：全部采样点地形高恒为 terrainHeight。
CameraSystem::TerrainAreaSampleFunc makeFlatAreaSampler(
    double terrainHeight) {
    return [terrainHeight](
               const Vec3& groundEcef, double /*radiusMeters*/,
               const std::vector<glm::dvec2>& offsets,
               std::vector<CameraSystem::TerrainSample>& out) {
        const auto& e = Ellipsoid::WGS84();
        const Cartographic c = e.cartesianToCartographic(groundEcef);
        const double cosLat =
            std::max(std::abs(std::cos(c.latitude())), 0.01);
        out.assign(offsets.size(), {});
        for (size_t i = 0; i < offsets.size(); ++i) {
            const double lat = c.latitude() + offsets[i].y / kEarthRadiusMeters;
            const double lon =
                c.longitude() + offsets[i].x / (kEarthRadiusMeters * cosLat);
            out[i].valid = true;
            out[i].heightMeters = terrainHeight;
            out[i].surfaceEcef = e.cartographicToCartesian(
                Cartographic(lon, lat, terrainHeight));
        }
    };
}

// 与 SceneFrameUpdateCoordinator 相同的公式（groundState 有效分支）。
double nearFromGroundState(const CameraSystem& c) {
    return std::max(
        CameraSystem::kNearFloorMeters,
        CameraSystem::kNearSafetyRatio *
            c.groundState().nearestGeometryMeters);
}

// 摆位：terrainHeight+agl 高度、朝正北下俯 pitchDeg 的自由位姿，随后
// update() 让帧末哨兵解算 groundState（外部裸写 → user-driven 立即路径）。
void placeCamera(Camera& camera, CameraSystem& controller,
                 double terrainHeight, double agl, double pitchDeg) {
    const auto& e = Ellipsoid::WGS84();
    const Vec3 eye = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.5, 29.6, terrainHeight + agl));
    const Vec3 up = e.geodeticSurfaceNormal(eye);
    const Vec3 north = (Vec3::unitZ() - up * up.dot(Vec3::unitZ())).normalized();
    const double p = pitchDeg * 3.14159265358979323846 / 180.0;
    const Vec3 dir = (north * std::cos(p) - up * std::sin(p)).normalized();
    camera.setView(eye, dir, up);
    controller.update(0.016);
}

}  // namespace

// near 在任何 AGL/俯角组合下都不得达到近场地形的真实最小距离（平地时 =
// 竖直 AGL）——掠视贴坡"看到山内部"的机器化判据。俯角覆盖 0°~90° 同时验证
// 公式与视线方向无关（探针旋转对称，无前向偏置 → 原地旋转 near 不抖）。
TEST(DynamicNearPlane, NearNeverClipsSampledTerrain) {
    const double terrainHeight = 1000.0;
    const double agls[] = {50.0, 500.0, 5000.0, 50000.0, 500000.0, 1.0e7};
    const double pitches[] = {0.0, 4.0, 30.0, 90.0};
    for (double agl : agls) {
        for (double pitch : pitches) {
            Camera camera;
            camera.setPerspective(1.0471975512, 1.0, 5.0e7);
            CameraSystem controller(&camera);
            controller.setViewport(800, 600);
            controller.setTerrainAreaSampleFunc(
                makeFlatAreaSampler(terrainHeight));
            placeCamera(camera, controller, terrainHeight, agl, pitch);

            ASSERT_TRUE(controller.groundState().valid)
                << "agl=" << agl << " pitch=" << pitch;
            const double nearPlane = nearFromGroundState(controller);
            EXPECT_GE(nearPlane, CameraSystem::kNearFloorMeters);
            // 平地最近几何 = 竖直 AGL；near 必须留出安全比余量。
            EXPECT_LE(nearPlane, CameraSystem::kNearSafetyRatio * agl +
                                     1e-6)
                << "agl=" << agl << " pitch=" << pitch;
        }
    }
}

// 高空（≥1000 km）新公式与旧椭球 nadir 公式相对差 <1% —— 动态紧 near 治
// z_ndc 病态区（planetary depth crack）的既有真机结论零回归的机器判据。
TEST(DynamicNearPlane, HighAltitudeNearMatchesLegacyFormula) {
    for (double agl : {1.0e6, 5.0e6, 1.0e7}) {
        Camera camera;
        camera.setPerspective(1.0471975512, 1.0, 5.0e7);
        CameraSystem controller(&camera);
        controller.setViewport(800, 600);
        controller.setTerrainAreaSampleFunc(makeFlatAreaSampler(0.0));
        placeCamera(camera, controller, 0.0, agl, 30.0);

        const double newNear = nearFromGroundState(controller);
        const double legacyNear = std::max(
            150.0,
            0.5 * (camera.position().length() -
                   Ellipsoid::WGS84().maximumRadius()));
        EXPECT_LT(std::abs(newNear - legacyNear) / legacyNear, 0.01)
            << "agl=" << agl;
    }
}

// 净空 ↔ near 下限耦合契约（编译期由头文件 static_assert 锁定，此处留
// 运行期可读断言）。
TEST(DynamicNearPlane, NearFloorRespectsClearanceCoupling) {
    EXPECT_GE(CameraSystem::kNearSafetyRatio *
                  CameraSystem::kMinClearanceMeters,
              CameraSystem::kNearFloorMeters);
}
