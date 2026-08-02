#pragma once

#include "earth_engine/sdk/EarthSceneConfig.h"

namespace earth_engine::minimal_globe_demo {

// 唯一地形源 = 规则栅格 raster-DEM 高度图地形（CPU 烘焙，TerrainSourceKind::Heightmap）。
// 65×65 顶点对齐 Mapbox Terrain-RGB PNG（dem_test build_raster_dem_grid65.py 生成）。
// QuantizedMesh / Cesium ion 路径已退役——heightmap 必须本地瓦片，**瓦片交付**（择一）：
//   ① 本地服务器：serve_tiles.py 起在 8091 + adb reverse tcp:8091，用下方 http 模板；
//   ② adb push + file://：把瓦片推到 app 可读目录，模板改 file:///<路径>/{z}/{x}/{y}.png。
// 本地自产 FABDEM raster-DEM(grid65 顶点栅格,仅覆盖重庆 ~700km 见方)。
// 需 serve_tiles.py@8091 + adb reverse tcp:8091。
constexpr const char* kHeightmapTerrainTemplate =
    "http://127.0.0.1:8091/{z}/{x}/{y}.png";

// 全球 NASA/Mapbox Terrain-RGB(514×514,cell-registered + 1px 重叠环,z6-12,
// 全球大部分覆盖)。直连 HTTPS(CA bundle 已内嵌),无需 adb reverse。用于测掠视/
// 大范围移动的加载体验——本地 FABDEM 覆盖太小,掠视视野大半落在数据外污染测量。
// 切换见 kUseGlobalTerrainSource。
constexpr const char* kGlobalTerrainTemplate =
    "https://mapoverlay.xinzhi.space/3dterrain/nasa/tiles/{z}/{x}/{y}.png";

// true = 用全球 NASA 源(测掠视加载体验);false = 本地重庆 FABDEM(默认生产/离线)。
constexpr bool kUseGlobalTerrainSource = true;

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
constexpr bool kMeasureFreezeCamera = false;

// kMeasureScriptedPan:测量台脚本化确定性平移(净测 §14.1② live 换页 ghost)。
// true = 相机从初始位姿起,每帧原地偏航固定增量、扫掠 kMeasureScriptedPanFrames
// 帧后 hold(见 CameraController::setScriptedPan)。方位角持续扫掠把新影像子瓦片
// 带进视野 → 逼 live page-in;帧计数确定性(内部计数,非 wall-clock)、无惯性 →
// 可复现受控运动,替 free swipe 惯性漂(漂到不可控 low-grazing 位姿)。配 PageDet/
// 逐帧截图量 ghost/stall。与 kMeasureFreezeCamera 互斥(freeze 优先)。
constexpr bool kMeasureScriptedPan = false;
constexpr int kMeasureScriptedPanStartFrame = 90;   // 扫掠前 hold 帧(让 settle)
constexpr int kMeasureScriptedPanFrames = 600;  // 扫掠帧数(慢扫,宽运动窗)
constexpr double kMeasureScriptedPanYawPerFrameDegrees = 0.1;   // 每帧偏航(600*0.1=60°)

// 注:北极星纹理/几何解耦(decoupleImageryFromGeometry)已升为生产主路径默认开
// (见 MinimalGlobeDemoConfig.cpp 中 config.tileset.decoupleImageryFromGeometry = true
// 及 §15.3⑤),不再由此处灰度常量门控;A/B 测耦合基线时直接改那行为 false。

// kMeasureVirtualTexturePoC:北极星 Phase 2b 虚拟纹理 C 方案 PoC(骨架)。
// true = 每帧旁路跑 feedback→回读→页表整链,把回读 stall 毫秒数报进 EarthPerf
// 头行(vtReadback=)。**这是量 C 移动端固定开销、回填设计 §5 诚实账、拍板
// §8 决策 #3(B vs C)的数据来源。** 不改任何渲染,纯测量。骨架局限:feedback
// pass 尚未接 page-id 片元 shader,故 vtVis 恒 0——回读 stall 计时依然有效。
constexpr bool kMeasureVirtualTexturePoC = false;

// kMeasureHorizonView:测量台视角切「斜视地平线」宽视野(低仰角+高空)。
// true 时用下方 horizon 常量覆盖 kMeasureObliqueElevationDegrees/kMeasureHeightMeters。
// 用途:验证「去耦后地平线视野下 capped 瓦片是否多到 B(逐瓦片合成)吃力」——
// 这是 B vs C 决策的关键场景(近景 capped 瓦片少 B 够用,地平线瓦片多则偏 C)。
// 仰角必须 ∈(0,90) 保持可复现(见相机可复现性注释);低仰角=望向地平线宽视野。
constexpr bool kMeasureHorizonView = false;
constexpr double kMeasureHorizonElevationDegrees = 10.0;
constexpr double kMeasureHorizonHeightMeters = 12000.0;

// kMeasureTileCompositeBakePoC:北极星 Phase 2b B 方案(逐瓦片合成)PoC。
// true = 每帧对当前可见瓦片数做 N 个离屏 bake pass,把烘焙耗时报进 EarthPerf
// 头行(bBake=)。**这是 B vs C 决策缺的第三块数据**——与 C 的 vtReadback 税对
// 比:近景 N=3(B 该赢) vs 地平线 N=122(C 该赢?)。配 kMeasureHorizonView 测
// 地平线。纯旁路测量,不改渲染。骨架量 pass 切换地板(合成 draw 待细化)。
constexpr bool kMeasureTileCompositeBakePoC = false;

// kMeasureVtIndirectionSamplePoC:北极星 Phase 2b 合成方案「门①」原型。
// true = 每帧一屏 fill 跑 baseline(1 次 atlas 采样)vs descent(N 次依赖间接
// fetch + atlas 采样),把逐片元间接采样倍率报进 EarthPerf 头行(vtiRatio=)。
// **这是合成方案唯一真未知(门①逐片元间接采样开销)的数据来源**——过门(倍率
// 与 B fill 同量级)→ 目标形态定合成方案;过不了 → 退 Option-lite。纯旁路测量。
constexpr bool kMeasureVtIndirectionSamplePoC = false;

// 注:北极星 SVT 页存储(terrainPageStore)已随 decouple 升为生产主路径默认开
// (见 MinimalGlobeDemoConfig.cpp 中 config.terrainPageStore = true 及 §15.3⑤),
// 不再由灰度常量门控;A/B 测时直接改那行为 false。

// 2026-06-10 14:00 UTC+8 = 06:00 UTC, +2.88h → 16:53 UTC+8。
//
// 偏移 +0.12 是**地形质感验收的前提**,不是随手调的:原值在重庆
// (106.508E, 29.617N)对应太阳高度角 **71.4°**(近天顶),NdotL≈0.948 →
// 方向项 clamp(NdotL·0.9+0.3) 恒饱和到 1.000。饱和态下任何法线来源、任何光照
// 曲线都产出同一个值,relief 改动一律测出"画面没变"——实测连续三次踩中
// (法线贴图 0.16%、光照曲线 8.37% 像素变化)。
// 详见 docs/issues/terrain-visual-maturity-gap-2026-08-02.md §4b.4。
//
// +0.12 日 → 太阳高度角 **34.3°**,NdotL=0.564,方向项 0.808(未饱和,且处于
// 线性响应区)。这也是地球引擎出图的惯例角度:斜射光才读得出地形起伏。
// 上界参考:高度角 ≥51°(NdotL≥0.778)即重新进入饱和,勿再调回。
constexpr double kFixedSimulationJulianDate = 2461188.75 + 0.12;

earth_engine::EarthSceneConfig makeDefaultDemoSceneConfig();

} // namespace earth_engine::minimal_globe_demo
