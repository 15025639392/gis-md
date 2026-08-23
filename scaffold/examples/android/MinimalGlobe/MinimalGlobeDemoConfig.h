#pragma once

#include "earth_engine/data/VectorRasterStyle.h"

#include "earth_engine/sdk/EarthSceneConfig.h"

#include <string>

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

// 矢量 demo 图层(P1 面/线 + P5b 标注 + P5a 编辑手柄 + P6c 300 点聚合)。
// 排查**地形本身**的加载/接缝观感时置 false —— 这些图层是贴地钳制的
// (ClampToGround),地形一重钳它们也跟着动,屏幕上的"接缝/跳变"未必是地形;
// 且 refreshClusterDisplay() 每帧跑一次聚合与标注避让,占主线程。
// 排除干扰是判因的前提,不是洁癖。
constexpr bool kEnableVectorDemoLayers = false;

// 海拔着色轨迹 demo(2026-08-23):一条带椭球高的 LineString 轨迹
// (FeatureStore 数据),按顶点海拔线性渐变烘进既有 a_color,复用
// VectorLine48 顶点布局与 shader(无新属性/着色器),lengthSoFar 照常
// 携带(dash 语义不变)。独立开关且默认开,不依赖上面整套 feature demo
// 图层 —— 排查地形时若嫌画面多一条线,置 false 即可。
constexpr bool kEnableElevationTrajectoryDemo = true;

// 矢量 P4 MVT 只读底图(**几何通路**)。本地 tippecanoe 自制重庆 OSM
// mbtiles,serve_mvt_tiles.py 起 8092 + adb reverse tcp:8092。
// 刀1(面→drape)后只承载线;刀2(线→SDF 场+地形 FS 解算)落地后线也
// 退役(2026-08-13)。**符号刀A(2026-08-14)复活本链为点符号通路**:
// layerRules 只放行 poi 层 → worker 出 TileSymbolCpu 实例表,准入定型
// billboard。至此三分工闭环:面=drape 页合成 / 线=SDF 场 / 点=本链。
// 要 A/B 对拍旧路网几何路径:includeLayers 加回 "roads" + roads 分级规则
// 塞回 layerRules(见 GLESView;⚠️ 整层白名单是 includeLayers,layerRules
// 未列出的层是全收)。获取层已与 drape/场共享 MvtTileFetchCache(刀A.5),
// 同一数据瓦网络/解码/内存恰一份。
constexpr bool kEnableMvtBasemap = true;

// 矢量**面** drape 底图:MVT 面要素动态栅格化冒充影像,进 TerrainPageStore
// 页合成(与卫星影像同轨,GPU 边际成本≈0)。E4 原版影像通路曾于 2026-08-07
// 整链删除(页纹素封顶,近景**线**糊成栅格块);本版按"面 drape/线 SDF"
// 新分工复活,overzoom 现画不再封顶,见 VectorDrapeImageryProvider.h。
// Metal 红利:drape 不依赖 stencil,iOS 首次获得贴地面能力。
constexpr bool kEnableMvtDrapeBasemap = true;

// 刀2 路网线 SDF 场:逐页 R8 距离场(CPU scatter 烘焙,worker)+ 地形 FS
// 内解算(smoothstep+fwidth 解析 AA,寄生地形 FS 边际成本≈0 —— S2 场税
// 探针实测增量在噪声内;独立 overlay pass 同数学 25-30ms,勿走回头路)。
// 与面 drape 共享 MvtTileFetchCache(同一批 z14 祖先瓦零重复 fetch)。
constexpr bool kEnableMvtRoadField = true;
/// E 方案路网几何通道(P1 接线,默认关:P2 的 VS 采高贴地落地前,路网在
/// 山地会飘在椭球面上;置 true 时与 D2 场互斥 —— RoadFieldSource 跳过)。
constexpr bool kEnableEPlanRoadRibbon = false;

// 贴地体的高度范围不在这里配:SceneRenderPipeline 每帧从**可见地形瓦片的
// 包围体**汇总(O(可见瓦片数),零采样),相机飞到哪都对。

/// 面 drape 通路的栅格样式(water/building 色块)。颜色与几何通路退役前的
/// fillColorExpr 对齐;线不在此配(走下方场解算样式)。
VectorRasterStyle makeMvtDrapeStyle();

/// 刀2 场解算的线样式(只消费 line 通道):highway 分级 + 线宽随页 zoom
/// 分档(styleZoom=页 z,跟屏幕清晰度走)。线色在 FS uniform 统一给出,
/// 见 kMvtRoadFieldColor。
VectorRasterStyle makeMvtRoadFieldStyle();
/// 场解算线色(RGBA 0-1,非预乘):对齐几何通路退役前的 bs.lineColor
/// (0.95,0.95,0.90,0.85),交接观感不变。
constexpr std::array<float, 4> kMvtRoadFieldColor{0.95f, 0.95f, 0.90f,
                                                  0.85f};

/// V26 一期换肤验证:夜间面样式(**调日版改色**,分级/filter 与日版同源,
/// 不复制 —— test_mvt_basemap_grading 锁的分级自动共享)。
VectorRasterStyle makeMvtDrapeStyleNight();
/// 夜间路网线色(亮琥珀):Uniform 成本类,经 setRoadFieldStyleUniforms
/// 零重烘切换。米白(日)↔琥珀(夜)肉眼即判。
constexpr std::array<float, 4> kMvtRoadFieldColorNight{1.00f, 0.72f, 0.20f,
                                                       0.90f};
// 路网场分级宽度 ramp (z0, halfPx0, z1, halfPx1):FS 在局部 zoom 上线性
// 插值线半宽(设备px),两端 clamp。宽度=0.6→1.8 CSS px × dpr 3.5 ÷ 2。
// zoom 基准=影像页 zoom(比地图直觉 zoom 高 ~2-3 档,30km 俯瞰全屏约
// 13→17,见 PageStoreSamplingGLSL.h),停点 12→16 让分级落在可见区间。
constexpr std::array<float, 4> kMvtRoadFieldWidthRampPx{12.0f, 1.05f,
                                                        16.0f, 3.15f};
// 场页 zoom 封顶 = max(MVT 数据 maxZoom=14, 样式最后一个 zoom 分级档=15
// (z>=15 catch-all 放开末梢路))。封小了末梢路整体消失(真机踩过封 14)。
constexpr int kMvtRoadFieldMaxZoom = 15;
constexpr const char* kMvtBasemapUrlTemplate =
    "http://127.0.0.1:8092/{z}/{x}/{y}.pbf";
constexpr int kMvtBasemapMinZoom = 0;
constexpr int kMvtBasemapMaxZoom = 14;

// MVT 数据瓦缓存两层容量(P2 结清,2026-08-15。实测:解码瓦 ~450KB/张、
// 压缩字节 ~33KB/张,13.6× 差)。
//   L1 = 48:够在途合并与热复用(三消费方并发要同一批祖先瓦),再大就是
//            每张 450KB 地烧内存 —— 48 张实测常驻 ~20-23MB。
//   L2 = 256:按"绕城一圈的工作集"取,~8.5MB。够大才能让网络重拉归零,
//            而这正是加 L1 容量买不起的那件事(同样覆盖要 ~115MB)。
constexpr size_t kMvtTileCacheDecoded = 48;
constexpr size_t kMvtTileCacheRaw = 256;

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
//     后续加"测量冻结相机"开关(冻 CameraSystem::update),Phase 2 前值得做。
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
// CameraSystem::update() 完全空转,相机逐帧字节稳定 → 即便高空重载耦合态
// (深影像 churn)的 far 位姿也精确可复现,让去耦前/后同位姿对拍成立(free-look
// 静止本已稳,但 far-5000 类重载 stop 偶尔仍漂,此开关彻底钉死)。测量一律开;
// 生产/交互路径保持 false(默认零影响)。
constexpr bool kMeasureFreezeCamera = false;

// kMeasureDisablePerTileRange:A/B 对照组 —— true 时贴地体高度范围退回**全屏
// 全局值**(逐瓦片局部范围改造前的行为)。纯测量开关,只在 MVT 镶嵌钩子那一处
// 生效。收窄倍率本身由 VectorClamp perTileRange 行报,这个开关量的是收窄换来
// 的 GPU 毫秒。⚠️先确认那行的 avg 明显 >1 再开 A/B,否则量的是噪声。
constexpr bool kMeasureDisablePerTileRange = false;

// kMeasureScriptedPan:测量台脚本化确定性平移(净测 §14.1② live 换页 ghost)。
// true = 相机从初始位姿起,每帧原地偏航固定增量、扫掠 kMeasureScriptedPanFrames
// 帧后 hold(见 CameraSystem::setScriptedPan)。方位角持续扫掠把新影像子瓦片
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

// kMeasureHorizonSunset:地平线视角切「日落」——相机看向日落太阳方位、时钟设到
// 太阳贴地平线的低角。用途:HDR 管线(kEnableHdrPipeline)大气 shader 线性化+
// 太阳 boost 的 payoff 对拍(太阳盘/大气 limb 辉光只有在太阳入画、低角时才显)。
// 仅在 kMeasureHorizonView=true 时生效。
//   azimuth 290.5° / JD 偏移 +0.21 → 重庆(106.508E,29.617N)太阳高度角 7.0°、
//   方位 WNW(见 SunDirection::compute + 手算);相机放 ESE 反侧看向 WNW 日落。
constexpr bool kMeasureHorizonSunset = false;
constexpr double kMeasureHorizonAzimuthDegrees = 290.5;
constexpr double kMeasureHorizonSunsetJulianOffset = 0.21;

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

// kMeasureGpuPassTiming:GPU 逐区间计时(GL_EXT_disjoint_timer_query)。
// true = 每秒一行 `GpuPass` 打进 logcat,把一帧 GPU 时间线切成
// pass.scene.clear / pass.terrainDepthPrepass / env / terrain / vec:<图层> /
// pass.postProcess 若干段。
// **这是"整机 GPU busy 86% 花在哪"唯一的直接证据来源** —— EarthPerf 头行量的
// 是提交命令的 CPU 成本,对 GPU 侧完全盲目,两者可以差一个数量级且互不预示。
// 判读的三条边界(TBDR 段边界不精确 / MSAA resolve 不在任何段内 / disjoint 帧
// 作废)见 renderer/GpuFrameTiming.h —— 不读那三条,这些数会被用来下错结论。
constexpr bool kMeasureGpuPassTiming = false;

// kBlackFrameProbe:黑块探针(漏底/黑块诊断)。swap 前逐帧回读降采样帧,
// 近黑(RGB 全 ≤8)占比 ≥0.5% 逐帧 Warning(logcat `BlackProbe`),300 帧
// 心跳报活。截图/录屏抽样会漏帧,这是唯一逐帧不漏的像素判据。含同步回读
// (~1-2ms/帧)——诊断会话才开。
// ⚠️ 判读:0.5% 阈值是近景标定;整球入画时太空+暗海的**合法基线 ~4%**,
// 命中须与 HoleQual drop 对照定性(黑块案实measured:真洞 16~51%,基线 4%)。
// 黑块案已结(根层常驻+预载,2026-08-10),平时关。
constexpr bool kBlackFrameProbe = false;

// kEnableFrameGating:帧级按需渲染。收敛后停止排帧,渲染线程真正睡下去。
// 静止是地图 app 的绝大多数时间,这段时间此前是逐 vsync 全量重建+重绘 ——
// 实测静止场景 GPU busy 86%、渲染线程 64% 单核,全部是纯浪费。
// ⚠️ 失效方向不是画错,是**画面冻住且零报错**:新接的异步产物若不置脏位,
// 它落地时没人消费。判据与接线见 Engine::setFrameGatingEnabled。
constexpr bool kEnableFrameGating = true;

// kEnableGpuHeightBake:B 方案——地形 height/normal 纹理 GPU 烘焙(替代 CPU
// bakeTerrainHeightNormalTexels ~6ms/瓦片)。**默认开**:GLES 已验证正确+无缝安全+
// 更顺滑(dense descent 顿挫 4/min→0)+热中性。后端守卫仅 GLES 走 GPU,Metal 回退
// CPU(MSL 待设备验证)。想 A/B 回 CPU 烘焙路径时置 false。
constexpr bool kEnableGpuHeightBake = true;

// kEnableSeamEdgeMismatchProbe:接边错位诊断探针(SeamDiag)。默认关 —— 常开
// 每帧 measure 约 4ms(真机 tether 实测,占 selPlan/update 大头),无缝北极星已
// 收官,探针只在再启动接边 A/B 时开。置 true 恢复逐帧累积 + 每 60 帧报告。
constexpr bool kEnableSeamEdgeMismatchProbe = false;

// kShadowVerifyIdle:影子渲染自检(方案 C)。gating 判定 idle 后不立刻睡,
// 继续渲 20 帧比对帧指纹 —— 画面在"应该静止"之后还变 = 有异步产物落地却
// 没人置脏位。本轮四次"零报错冻屏"里唯一有普适性的守卫(它不关心是哪个
// 子系统漏了,只看结果)。
// ⚠️ **默认关,且严禁在性能测量时开启**:自检帧要跑同步 glReadPixels,在
//    TBDR 上是管线 flush,会污染同一会话里的所有帧时/GPU 读数。
// ⚠️ 开之前先确认画面**真的**该静止:时钟已由 kFixedSimulationJulianDate
//    冻住,但任何逐帧抖动/jitter 效果都会让它一直报警 —— 而"一直报警"比
//    没有守卫更糟,人会学会无视它。
// 2026-08-18 默认开启:V27 家族五洞(placement/fade/字形烘焙/重钳/换代)全是
// "依赖帧循环的收敛没申报 → 停帧饿死"同构病,症状零报错。本自检是这类漏报
// 的**构造性捕网**——任何子系统忘了申报,idle 前画面仍在变,它就报警
// (ShadowVerify Error 行)。真机验收流程应看它的 changedFrames 读数(健康=无 Error 行)。
//
// ⚠️ 2026-08-19 改为 **仅 debug 构建默认开**(NDEBUG 未定义时)。上面 line 277
// "严禁性能测量时开启"与"默认开启"本自相矛盾——性能测量/生产必须 release。
// 且"成本只在 idle 一次、稳态零"是**快机直觉,弱机被真机推翻**:V1818T
// (GPU 积压 ~124ms)上,验证窗口每帧的同步 glReadPixels 排空整条管线 = **261ms/帧
// × 20 帧 = 松手后 ~5s 顿挫**(交互卡根因排查实测,readback 排的是 CPU 抢跑出来的
// 多帧积压)。故:debug 保留捕网(dev 期抓收敛漏报),release/perf/production 关闭
// (帧时/GPU 读数不被污染、交互无 readback 顿挫)。
// **要在 release 上跑 ShadowVerify 验收**:编 debug 变体,或临时把本块改成恒 true。
#ifdef NDEBUG
constexpr bool kShadowVerifyIdle = false;  // release / perf / production:关
#else
constexpr bool kShadowVerifyIdle = true;   // debug / dev:开(帧收敛漏报捕网)
#endif

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

// ---- V26 尾项:数据源 URL 启动期外置(2026-08-18) ----
//
// 设备侧 sources.json(与 style-*.json 同目录约定)覆盖编译期 URL 模板,
// 换城市/换源不再重编译。**只在启动装配时读一次**——运行期热切源刻意
// 不做(provider 重建 + 缓存失效 + V28 换手扩面,是独立专项的体量)。
// zoom 范围等源参数仍归编译期(fail-loud:文档出现未知键整份拒收)。
//
// JSON 形态(全部键可选,缺 = 用内置):
//   { "mvtUrlTemplate":     "http://127.0.0.1:8092/{z}/{x}/{y}.pbf",
//     "imageryUrlTemplate":  "...{x}...{y}...{z}...",
//     "terrainUrlTemplate":  "...{z}/{x}/{y}.png" }
struct DemoSourceOverrides {
    std::string mvtUrlTemplate;      ///< 空 = 内置 kMvtBasemapUrlTemplate
    std::string imageryUrlTemplate;  ///< 空 = 内置(高德/本地按既有 flag)
    std::string terrainUrlTemplate;  ///< 空 = 内置(全球/本地按既有 flag)
};

/// 解析 sources.json。fail-loud 同 StyleDocument 课:未知键 / 非字符串值 /
/// 坏 JSON → 返回 false 且 outError 给人话,out 不动(调用方回落内置)。
bool parseDemoSourceOverrides(const std::string& jsonText,
                              DemoSourceOverrides& out,
                              std::string& outError);

/// overrides 非空字段覆盖 terrain/imagery URL;传 nullptr = 全内置。
/// (MVT URL 的消费点在 GLESView 的 fetch 闭包,不经 SceneConfig,由
/// 调用方自取 mvtUrlTemplate。)
earth_engine::EarthSceneConfig makeDefaultDemoSceneConfig(
    const DemoSourceOverrides* overrides = nullptr);

} // namespace earth_engine::minimal_globe_demo
