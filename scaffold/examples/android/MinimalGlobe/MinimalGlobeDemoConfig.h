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

// 实例化性能基线(默认关,见 kEnableInstancedI3dmDemo):Cesium 官方
// 3d-tiles-samples 的 tree.i3dm(gltfFormat=1 内嵌 glb, EAST_NORTH_UP, 树干
// OPAQUE + 树叶 BLEND)。经 SingleGltfContentProvider 的 .i3dm 解码 →
// GltfPrimitiveInstanced 实例化绘制,验证 blend 实例化走 alpha-to-coverage 单
// draw(不退化成逐实例 draw 爆炸)。境外网络受限设备上测量时,改指 adb reverse
// 的 localhost(http://127.0.0.1:8091/treeN.i3dm,scratchpad 按 N 生成缩放变体)。
constexpr const char* kTreeI3dmUrl =
    "https://raw.githubusercontent.com/CesiumGS/3d-tiles-samples/main/"
    "1.0/TilesetWithTreeBillboards/tree.i3dm";

constexpr bool kEnableTerrainForDemo = true;
constexpr bool kUseGaodeSatelliteForDemo = true;
constexpr bool kEnableGaodeRoadNetOverlayForDemo = false;
constexpr bool kEnableRobotExpressiveGltfDemo = false;
// 开启后 config.gltf 指向 tree.i3dm(覆盖 RobotExpressive),用于实例化基线测量。
// tree.i3dm 是世界锚定内容,相机随之移到样本真实位置(宾州)低空俯视。
// 默认关:依赖 localhost 服务器+生成变体,非独立可跑;开启前先起 http.server。
constexpr bool kEnableInstancedI3dmDemo = false;
// tree.i3dm 25 实例质心的真实经纬度(绝对 ECEF 反算, 簇径~180m)。
constexpr double kTreeI3dmLongitudeDegrees = -75.612094;
constexpr double kTreeI3dmLatitudeDegrees = 40.042531;
// 低空俯视: 180m 簇在此高度铺满可观屏幕面积, 保证实例化 fragment 开销可测。
constexpr double kTreeI3dmCameraHeightMeters = 350.0;

constexpr double kDemoCameraLongitudeDegrees = 106.508;
constexpr double kDemoCameraLatitudeDegrees = 29.617;
constexpr double kDemoCameraHeightMeters = 30000.0;

// 2026-06-10 14:00 UTC+8 = 06:00 UTC.
constexpr double kFixedSimulationJulianDate = 2461188.75;

earth_engine::EarthSceneConfig makeDefaultDemoSceneConfig();

} // namespace earth_engine::minimal_globe_demo
