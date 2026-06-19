#pragma once

#include "earth_engine/sdk/EarthSceneConfig.h"

namespace earth_engine::minimal_globe_demo {

constexpr const char* kFabdemTerrainTemplate =
    "http://192.168.1.4:8001/{z}/{x}/{y}.png";
constexpr const char* kQuantizedMeshTerrainTemplate =
    "http://192.168.1.8:8090/{z}/{x}/{y}.terrain";
constexpr const char* kQuantizedMeshTerrainLayerJson =
    "http://192.168.1.8:8090/layer.json";
constexpr const char* kGaodeSatelliteTemplate =
    "http://webst0{s}.is.autonavi.com/appmaptile?style=6&x={x}&y={y}&z={z}";
constexpr const char* kGaodeRoadNetTemplate =
    "http://webst0{s}.is.autonavi.com/appmaptile?style=8&x={x}&y={y}&z={z}";
constexpr const char* kRobotExpressiveGlbUrl =
    "https://maptalks.org/maptalks.three/demo/data/RobotExpressive.glb";

constexpr bool kEnableTerrainForDemo = true;
constexpr bool kUseGaodeSatelliteForDemo = true;
constexpr bool kEnableGaodeRoadNetOverlayForDemo = true;
constexpr bool kEnableRobotExpressiveGltfDemo = false;
/// Use QuantizedMesh terrain (cesium-native format) instead of RGB heightmap.
constexpr bool kUseQuantizedMeshTerrain = true;

constexpr double kDemoCameraLongitudeDegrees = 106.508;
constexpr double kDemoCameraLatitudeDegrees = 29.617;
constexpr double kDemoCameraHeightMeters = 30000.0;

// 2026-06-10 14:00 UTC+8 = 06:00 UTC.
constexpr double kFixedSimulationJulianDate = 2461188.75;

earth_engine::EarthSceneConfig makeDefaultDemoSceneConfig();

} // namespace earth_engine::minimal_globe_demo
