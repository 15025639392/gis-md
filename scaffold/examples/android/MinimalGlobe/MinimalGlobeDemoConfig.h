#pragma once

#include "earth_engine/sdk/EarthSceneConfig.h"

namespace earth_engine::minimal_globe_demo {

// 本地自建 quantized-mesh（重庆 FABDEM，经 adb reverse → 127.0.0.1:8090）。
// kUseCesiumIonTerrain=false 时回退到这套。
constexpr const char* kQuantizedMeshTerrainTemplate =
    "http://127.0.0.1:8090/{z}/{x}/{y}.terrain";
constexpr const char* kQuantizedMeshTerrainLayerJson =
    "http://127.0.0.1:8090/layer.json";

// Cesium ion World Terrain（asset 1）：全球地形，走 ion endpoint 运行时协商，
// 脱离本地服务器 + adb reverse 依赖。临时凭证约 1 小时过期（当前无自动刷新）。
constexpr bool kUseCesiumIonTerrain = true;
constexpr int kCesiumIonTerrainAssetId = 1;
constexpr const char* kCesiumIonAccessToken =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."
    "eyJqdGkiOiI5YzU4ODg1YS01ZTY3LTRhODYtOGUyZi04ZjkxYjg0OTg2ZGMiLCJpZCI6NjM2"
    "MDUsImlhdCI6MTYyODMxNzM2OH0."
    "McSbZHfB5rO3TmWDHRGFdUWVayCvs8iiKuAUyPoYJyY";
constexpr const char* kGaodeSatelliteTemplate =
    "http://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}";
constexpr const char* kGaodeRoadNetTemplate =
    "http://webst0{s}.is.autonavi.com/appmaptile?style=8&x={x}&y={y}&z={z}";
constexpr const char* kRobotExpressiveGlbUrl =
    "https://maptalks.org/maptalks.three/demo/data/RobotExpressive.glb";

constexpr bool kEnableTerrainForDemo = true;
constexpr bool kUseGaodeSatelliteForDemo = true;
constexpr bool kEnableGaodeRoadNetOverlayForDemo = false;
constexpr bool kEnableRobotExpressiveGltfDemo = false;

constexpr double kDemoCameraLongitudeDegrees = 106.508;
constexpr double kDemoCameraLatitudeDegrees = 29.617;
constexpr double kDemoCameraHeightMeters = 30000.0;

// 2026-06-10 14:00 UTC+8 = 06:00 UTC.
constexpr double kFixedSimulationJulianDate = 2461188.75;

earth_engine::EarthSceneConfig makeDefaultDemoSceneConfig();

} // namespace earth_engine::minimal_globe_demo
