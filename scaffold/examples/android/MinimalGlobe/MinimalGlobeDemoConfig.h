#pragma once


#include "earth_engine/sdk/EarthSceneConfig.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace earth_engine::minimal_globe_demo {

struct AmapWorkerBudget {
    size_t decodeThreads = 1;
    size_t poiDecodeThreads = 1;
    size_t tessellationThreads = 1;
};

/// 为手机保留渲染/系统核心，并按内存约束限制同时存活的解码/网格临时对象。
/// 低内存机并发保守，但 type1/POI 解码仍有独立通道；
/// 8 核高内存机使用 2+1+3，总后台线程不超过扣除前台保留后的预算。
AmapWorkerBudget chooseAmapWorkerBudget(int cpuCores,
                                        int64_t totalMemoryBytes);


// 海拔着色轨迹 demo(2026-08-23):一条带椭球高的 LineString 轨迹
// (FeatureStore 数据),按顶点海拔线性渐变烘进既有 a_color,复用
// VectorLine48 顶点布局与 shader(无新属性/着色器),lengthSoFar 照常
// 携带(dash 语义不变)。独立开关且默认开,不依赖上面整套 feature demo
// 图层 —— 排查地形时若嫌画面多一条线,置 false 即可。
constexpr bool kEnableElevationTrajectoryDemo = false;

// 官方 AMap type-1/type-2 数据瓦缓存两层容量。实测:解码瓦 ~450KB/张、
// 压缩字节 ~33KB/张,13.6× 差)。
//   L1 = 48:够在途合并与热复用(三消费方并发要同一批祖先瓦),再大就是
//            每张 450KB 地烧内存 —— 48 张实测常驻 ~20-23MB。
//   L2 = 256:按"绕城一圈的工作集"取,~8.5MB。够大才能让网络重拉归零,
//            而这正是加 L1 容量买不起的那件事(同样覆盖要 ~115MB)。
constexpr size_t kAmapTileCacheDecoded = 48;
constexpr size_t kAmapTileCacheRaw = 256;
// 官方 AMap 后台工作预算。解码和镶嵌使用独立池，避免前面的几何镶嵌
// 把后续 POI/底图解码挡在 FIFO 后面。这里是无设备信息时的保守回退；
// Android 真机按 cpu 核数/内存动态计算。两池合计有界，不减少可见瓦片。
constexpr size_t kAmapType1DecodeThreadsFallback = 2;
constexpr size_t kAmapTessellationThreadsFallback = 2;

// 用户指示先隐藏卫星底图,避免卫星盖住矢量层造成的误导。
// 地形暂保留(amap 复刻平面语义最终要关,但关地形会触发另一个
// 启动即卡问题,单独排查后再切)。
/// 仅用于诊断 fill 层级的临时 A/B 开关。生产默认显示建筑；隐藏仍保留
/// 下载/解码成本，因此不能作为性能策略。
// amap.com 默认二维日间底图不以建筑体块为主视觉。隐藏深色建筑填充，
// 让道路、行政边界、水系和标注建立主要信息层级。
/// 高德矢量:type2 面走 VectorFill(z10 粗源,V30 地球网格)，type1/3
/// 路网+建筑走主源 FeatureRenderLayer。MinimalGlobe 只提供这一套官方
/// pure-vector basemap 合同，不保留关闭 runtime 后落入通用路径的开关。
/// 高德 web key(dev;产品换 key 只改这里)。Referer 由官方 runtime 固定，
/// 应用层不能覆盖 transport 合同。
constexpr const char* kAmapWebKey =
    "14656ce3418e226459ecead9f67c7681";

// === 官方 Amap 场景真实地形源 ===
// 历史两源:全球 NASA Terrain-RGB(直连,覆盖全球)与本地 FABDEM
// (127.0.0.1:8091,离线)。生产默认用全球 NASA(2026-09-01)。
constexpr const char* kAmapGlobalTerrainTemplate =
    "https://mapoverlay.xinzhi.space/3dterrain/nasa/tiles/{z}/{x}/{y}.png";
constexpr int kAmapTerrainMinZoom = 6;
constexpr int kAmapTerrainMaxZoom = 12;
constexpr int kAmapTerrainTileSize = 514;
constexpr float kAmapTerrainBorderInset = 0.5f;

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
// 城市级视野便于与 amap.com 默认重庆页面直接比较；1500m 只覆盖园区，
// 无法评价城市地名、干线路网和水系层级。
constexpr double kMeasureHeightMeters = 30000.0;
// amap.com 是二维正俯视地图；纯矢量验收默认使用接近 nadir 的视角，
// 避免 3D 地球的倾斜透视成为与官网的主要差异。
constexpr double kMeasureObliqueElevationDegrees = 89.0;
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

earth_engine::EarthSceneConfig makeDefaultDemoSceneConfig();

} // namespace earth_engine::minimal_globe_demo
