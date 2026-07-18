#pragma once

#include "earth_engine/sdk/EarthSceneConfig.h"

namespace earth_engine::minimal_globe_demo {

// 唯一地形源 = 规则栅格 raster-DEM 高度图地形（CPU 烘焙，TerrainSourceKind::Heightmap）。
// 65×65 顶点对齐 Mapbox Terrain-RGB PNG（dem_test build_raster_dem_grid65.py 生成）。
// QuantizedMesh / Cesium ion 路径已退役——heightmap 必须本地瓦片，**瓦片交付**（择一）：
//   ① 本地服务器：serve_tiles.py 起在 8091 + adb reverse tcp:8091，用下方 http 模板；
//   ② adb push + file://：把瓦片推到 app 可读目录，模板改 file:///<路径>/{z}/{x}/{y}.png。
constexpr const char* kHeightmapTerrainTemplate =
    "http://127.0.0.1:8091/{z}/{x}/{y}.png";

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

// === 北极星测量台:编译期钉死相机(改 kMeasure* 常量→重建→采一个 stop)。
// 同一点(重庆)变高度做 zoom 梯度 + 改影像 maxZoom 做耦合/去耦对拍。
// heightMeters = 眼睛离椭球面(海拔 0)高度;重庆地表 ~300m,最小离地 clamp 50m。
//
// ⭐ 相机可复现性(踩坑教训,务必遵守):
//   • obliqueElevationDegrees ∈ (0,90) = **free-look 模式,精确可复现**——每帧
//     update() 早退不动相机,静止无 clamp。实测 elev=45 两次启动逐位一致
//     (camH/pitch/heading 完全相同)。**测量一律用此,推荐 pitch=-45(elev=45)。**
//   • obliqueElevationDegrees = 0 = orbit 模式,**不可复现**:每帧重建 orbit +
//     地形 clamp,settled 位姿随地形加载态漂移。**别用 0 测量。**
//   • elev=90 退化(up∥viewDir 基座塌陷,朝向乱)。用 45,别用 90。
//   • 重载耦合态(高空 + 深影像 churn)偶尔仍会漂(见 far-5000 stop);彻底稳需
//     后续加"测量冻结相机"开关(冻 CameraController::update),Phase 2 前值得做。
//   • CamPose logcat 行(GLESView 每帧采样打)= 相机位姿真值,采集时读它校验钉死。
//
// kMeasureImageryMaxZoom:高德影像 maxZoom。=18 耦合态(影像逼地形假细分到 z13-18);
//   =12(=地形 native cap)则影像不再驱动上采样→隔离出"地形假细分"资源成本。
//   同一相机位姿下 z18 vs z12 对拍 = 干净测出耦合浪费。生产默认 18。
constexpr double kMeasureLongitudeDegrees = 106.508;
constexpr double kMeasureLatitudeDegrees = 29.617;
constexpr double kMeasureHeightMeters = 1500.0;
constexpr double kMeasureObliqueElevationDegrees = 45.0;
constexpr int kMeasureImageryMaxZoom = 18;

// kMeasureFreezeCamera:测量台冻结相机。true = 初始位姿设定后
// CameraController::update() 完全空转,相机逐帧字节稳定 → 即便高空重载耦合态
// (深影像 churn)的 far 位姿也精确可复现,让去耦前/后同位姿对拍成立(free-look
// 静止本已稳,但 far-5000 类重载 stop 偶尔仍漂,此开关彻底钉死)。测量一律开;
// 生产/交互路径保持 false(默认零影响)。
constexpr bool kMeasureFreezeCamera = true;

// kMeasureDecoupleImageryFromGeometry:北极星 Phase 2a 断纹理/几何耦合(flag 灰度)。
// false = 耦合态(忠实 cesium,影像 isMoreDetailAvailable 捏造上采样地形子瓦片,
//   瓦片数爆 22×,= 去耦前对照列)。
// true  = 断耦合,几何 cap 在 DEM native max LOD(z12),影像不再驱动 refine,近景
//   影像走 scale-bias 祖先复用(暂糊,Phase 2b 补清)。同位姿(配 kMeasureFreezeCamera)
//   对拍 off/on 两列瓦片数/selector/churn = 干净测出解耦收益。
constexpr bool kMeasureDecoupleImageryFromGeometry = false;

// 2026-06-10 14:00 UTC+8 = 06:00 UTC.
constexpr double kFixedSimulationJulianDate = 2461188.75;

earth_engine::EarthSceneConfig makeDefaultDemoSceneConfig();

} // namespace earth_engine::minimal_globe_demo
