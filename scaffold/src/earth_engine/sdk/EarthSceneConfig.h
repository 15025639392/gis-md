#pragma once

#include "../layers/RasterOverlay.h"
#include "../layers/FeatureRenderLayer.h"
#include "../data/StyleFilter.h"
#include "../data/VectorTileTree.h"
#include "../data/MvtTileFetchCache.h"
#include "../platform/bridge/PlatformBridge.h"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace earth_engine {

enum class TerrainSourceKind {
    None,
    // 规则栅格高度图（raster DEM）地形：web-mercator XYZ 瓦片，每片一张
    // tileSize×tileSize 的高程栅格（Mapbox Terrain-RGB 或 Terrarium PNG 编码），
    // CPU 烘焙成规则栅格 glTF 网格（复用整条现有渲染管线）。
    Heightmap
};

// 高度图 RGB 编码方式（对应 HeightmapTerrainProvider::Encoding）。
enum class TerrainHeightmapEncoding {
    // height(m) = -10000 + (R*65536 + G*256 + B) * 0.1
    MapboxTerrainRgb,
    // height(m) = R*256 + G + B/256 - 32768
    Terrarium
};

enum class ImagerySourceKind {
    Debug,
    Xyz,
    TileMapService,
    WebMapService,
    WebMapTileService,
    BingMaps,
    GoogleMapTiles
};

struct SceneCameraConfig {
    double longitudeDegrees = 0.0;
    double latitudeDegrees = 0.0;
    double heightMeters = 0.0;
    // 初始视角仰角(度): 0 = 正俯视(nadir orbit, 默认/向后兼容); >0 = 自由斜视,
    // 相机从目标点正南、以该仰角俯瞰(适合看竖直 foliage 等地表小物, nadir 下隐形)。
    double obliqueElevationDegrees = 0.0;
    // 斜视视角的水平朝向(度, 0=正北[默认/向后兼容], 90=东, 180=南, 270=西)。
    // 仅在 obliqueElevationDegrees>0 时生效:相机放在该方位的**反侧**、看向该方位。
    // 默认 0 = 相机在正南看向正北(= 旧行为逐字等价)。测量台把日落太阳摆进画面
    // 时,设成太阳方位角(见 kMeasureHorizonAzimuthDegrees)。
    double obliqueAzimuthDegrees = 0.0;
    // 北极星测量台冻结相机: true 时初始位姿设定后 CameraSystem::update() 完全
    // 空转，相机逐帧字节稳定，让重载耦合态下的 far 位姿也精确可复现（去耦对拍需
    // 同位姿）。默认 false = 生产交互路径零影响。
    bool freezeCamera = false;
    // 北极星测量台脚本化确定性平移(§14.1② live 换页净测):true 时初始位姿设定后
    // 相机每帧原地偏航一步、共 scriptedPanFrames 帧后 hold(见 CameraSystem::
    // setScriptedPan)。给可复现受控运动量 ghost/stall,替 free swipe 惯性漂。
    // 与 freezeCamera 互斥(freeze 优先)。默认 false = 生产交互路径零影响。
    bool scriptedPan = false;
    int scriptedPanStartFrame = 0;  // 扫掠前先 hold 的帧数(让冷启动 settle 到 crisp)
    int scriptedPanFrames = 0;
    double scriptedPanYawPerFrameRad = 0.0;
};

struct TerrainSourceConfig {
    TerrainSourceKind kind = TerrainSourceKind::None;
    std::string urlTemplate;
    std::string attribution;
    int minimumZoom = 0;
    int maximumZoom = 0;
    int tileSize = 0;
    // 无细数据区回落椭球（partial 覆盖数据集）：主源无数据的瓦片在
    // [0, ellipsoidFallbackMaxZoom] 内回落为平滑椭球面，而非停成粗叶子 +
    // 巨型 skirt 裙墙。默认关 = 全局数据集零改动零回归，椭球源永不命中。
    // maxZoom 界定纯空洞区的细化深度（椭球是平的，无需深细化）。
    bool ellipsoidFallback = false;
    int ellipsoidFallbackMaxZoom = 13;
    int ellipsoidFallbackGridSize = 16;

    // 高度图地形字段（kind == Heightmap 时用；复用 urlTemplate/minimumZoom/
    // maximumZoom/attribution）。网格分辨率由解码后的 heightmap 尺寸自动决定
    // （gridSize = tileSize − 1），不单独配置。
    TerrainHeightmapEncoding heightmapEncoding =
        TerrainHeightmapEncoding::MapboxTerrainRgb;
    float heightmapHeightFactor = 1.0f;
    std::vector<float> heightmapNoDataValues;
    // 边界内缩(像素),透传到 HeightmapTerrainProvider::setBorderInset →
    // DecodedHeightmap::borderInset。0 = 顶点栅格源(自产 grid65,像素落在瓦片
    // 边界上);0.5 = cell-registered + 1px 重叠环(Mapbox 514 Terrain-RGB,真实
    // 边界在半像素处,内缩后相邻瓦片无缝)。见 DecodedHeightmap::borderInset。
    float heightmapBorderInset = 0.0f;
    // 原生最大 zoom（0 → 取 maximumZoom）。超出后由上采样（父级重采样）供给。
    int heightmapMaxNativeZoom = 0;
};

struct SceneTilesetConfig {
    // 与 TilesetOptions::mainThreadLoadingTimeLimit 默认保持一致（8ms 时间闸门）。
    double mainThreadLoadingTimeLimit = 8.0;
    double tileCacheUnloadTimeLimit = 0.0;
    int64_t maximumCachedBytes = 512LL * 1024 * 1024;
    // 运动期跳过快速划走的瓦片网络请求(cesium-js cullRequestsWhileMoving)。
    // 默认关 = 忠实 cesium-native。开启后拖动/缩放中减少瞬时加载洪泛,相机停下
    // 恢复正常加载。multiplier 越大剔除越激进(cesium 默认 60)。
    bool cullRequestsWhileMoving = false;
    double cullRequestsWhileMovingMultiplier = 60.0;
    // 地形 fill 代理(cesium-js TerrainFillMesh):可见瓦片真实地形未到时先贴椭球
    // 代理网格,影像立即显示在平滑地球上,真实地形到达再"隆起"。默认关。
    bool enableTerrainFillProxy = false;
    int terrainFillProxyGridSize = 16;
    // 北极星 Phase 2a 断纹理/几何耦合(flag 灰度):默认 false = 忠实 cesium
    // (影像 isMoreDetailAvailable 捏造上采样地形子瓦片,几何随影像细分,瓦片数
    // 爆 22×)。置 true = 几何按 DEM 几何误差细化,cap 在 native max LOD(z12);
    // 影像不再驱动 refine,近景影像走 scale-bias 祖先复用(暂糊,Phase 2b 补清)。
    bool decoupleImageryFromGeometry = false;
    // 接边错位诊断探针(SeamDiag)。无缝北极星已收官,探针只为再启动接边 A/B;
    // 常开每帧 measure 约 4ms(selPlan 大头,真机 tether 实测),故默认关。
    // 置 true 恢复逐帧累积 + 每 60 帧 SeamDiag 报告。见 TileRenderPlanFrameRefresher。
    bool seamEdgeMismatchProbe = false;
};

struct RasterOverlaySourceConfig {
    ImagerySourceKind imageryKind = ImagerySourceKind::Xyz;
    std::string urlTemplate;
    std::string tileMapResourceUrl;
    std::string attribution;
    int minimumZoom = 0;
    int maximumZoom = 0;
    int overlayMinimumZoom = 0;
    int overlayMaximumZoom = 0;
    int maximumSimultaneousTileLoads = 20;
    double maximumScreenSpaceError = 2.0;
    float opacity = 1.0f;
    RasterOverlayRole role = RasterOverlayRole::BaseImagery;
    RasterOverlayPriority priority = RasterOverlayPriority::High;
    RasterOverlayFallbackPolicy fallbackPolicy =
        RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
    bool blocksCompleteRenderable = true;
    RasterOverlayGeoreference georeference =
        RasterOverlayGeoreference::Standard;
    std::string wmsVersion = "1.3.0";
    std::string wmsLayers;
    std::string wmsFormat = "image/png";
    int imageryTileWidth = 0;
    int imageryTileHeight = 0;
    std::string wmtsFormat;
    std::string wmtsLayer;
    std::string wmtsStyle;
    std::string wmtsTileMatrixSetId;
    std::string wmtsSchemeId = "XYZ-WebMercator";
    std::vector<std::string> wmtsTileMatrixLabels;
    std::vector<std::string> wmtsSubdomains;
    std::map<std::string, std::string> wmtsDimensions;
    std::string bingBaseUrl;
    std::string bingMapStyle = "Aerial";
    std::string bingKey;
    std::string bingCulture;
    std::vector<std::string> bingSubdomains;
    std::string googleMapTilesApiBaseUrl = "https://tile.googleapis.com/";
    std::string googleMapTilesKey;
    std::string googleMapTilesSession;
    std::string googleMapTilesMapType = "satellite";
    std::string googleMapTilesLanguage = "en-US";
    std::string googleMapTilesRegion = "US";
    std::optional<std::string> googleMapTilesImageFormat;
    std::optional<std::string> googleMapTilesScale;
    std::optional<bool> googleMapTilesHighDpi;
    std::optional<std::vector<std::string>> googleMapTilesLayerTypes;
    std::optional<nlohmann::json> googleMapTilesStyles;
    std::optional<bool> googleMapTilesOverlay;
};

struct GltfSourceConfig {
    bool enabled = false;
    std::string tileSchemeId = "Geographic-TMS";
    int tileLevel = 0;
    int tileX = 1;
    int tileY = 0;
    std::string url;
    std::string name;
    double longitudeDegrees = 0.0;
    double latitudeDegrees = 0.0;
    double heightMeters = 0.0;
    double uniformScale = 1.0;
    // true = 内容自带世界坐标(如 i3dm 绝对 ECEF 实例位置),不套 ENU 就地放置,
    // 否则 ENU(lon/lat/height)*scale 会与内容自身锚定二次叠加把它推到界外。
    bool worldAnchored = false;
};

/// A Scene-owned MVT source rendered through its own FeatureRenderLayer.
/// Every entry receives an independent decoded/raw cache namespace by default,
/// so equal z/x/y coordinates from different URLs cannot alias.  Supplying
/// sharedCache is an explicit opt-in to a provider namespace shared with other
/// consumers.
struct MvtSourceConfig {
    std::string id;
    /// Standard MVT URL template.  The built-in converter currently expects
    /// XYZ WebMercator tile coordinates; {z}, {x}, {y}, and {s} are expanded by
    /// the SDK default fetcher.  Non-XYZ projections require a custom fetch /
    /// conversion path and are intentionally not inferred from the URL.
    std::string urlTemplate;
    /// Values used for {s}, selected deterministically by tile coordinate.
    /// Defaults preserve the prior numeric 0..3 convention; use {"a","b","c"}
    /// for common lettered XYZ hosts. Empty falls back to "0".
    std::vector<std::string> subdomains = {"0", "1", "2", "3"};
    int minimumZoom = 0;
    int maximumZoom = 14;
    size_t maximumCachedTiles = 300;
    int maximumTilesPerView = 64;
    int maximumPendingRequests = 64;
    double zoomBias = 0.0;
    VectorTileRefinementPolicy refinement =
        VectorTileRefinementPolicy::SymbolAdditive;
    std::vector<std::string> includeLayers;
    std::vector<SourceLayerRule> layerRules;
    size_t maximumTileCommitsPerUpdate = 4;
    size_t maximumTessellationsInFlight = 0;
    size_t decodedCacheTiles = 48;
    size_t rawCacheTiles = 256;
    size_t decodeThreads = 1;
    size_t tessellationThreads = 1;
    HttpRequestPriority requestPriority = HttpRequestPriority::Low;
    /// Optional custom fetcher. When empty, SDK uses PlatformBridge::get with
    /// {z}/{x}/{y} URL substitution and owns the request lifetime.
    MvtTileFetchCache::FetchFn fetchTile;
    /// Explicitly shared cache for consumers of the same provider namespace
    /// (for example raster drape + symbol source). Empty = source-local cache.
    std::shared_ptr<MvtTileFetchCache> sharedCache;
    FeatureRenderStyle style;
};

struct EarthSceneConfig {
    /// Initial SDK scene camera. resetCamera() restores this viewpoint.
    SceneCameraConfig initialCamera;
    /// Terrain source. TerrainSourceKind::None keeps the ellipsoid surface.
    TerrainSourceConfig terrain;
    /// Tileset runtime tuning. Defaults keep core Tileset defaults.
    SceneTilesetConfig tileset;
    /// Ordered raster stack. Ownership becomes SDK runtime state on install.
    std::vector<RasterOverlaySourceConfig> rasterOverlays;
    /// Ordered MVT sources. Runtime ownership is transferred to Scene.
    std::vector<MvtSourceConfig> mvtSources;
    /// Optional single glTF source, placed east-north-up on a configured tile.
    GltfSourceConfig gltf;
    /// Julian date set during installScene(); 0.0 leaves the epoch value.
    double fixedSimulationJulianDate = 0.0;
    /// 离屏 passthrough(RTT 冒烟通路,默认关):场景画进离屏 FBO 再全屏
    /// blit 上屏,像素应与直绘一致。用于点亮/守护 createFramebuffer +
    /// beginPass 离屏渲染通路(后处理链地基)。当前仅 GLES 生效。
    bool debugOffscreenPassthrough = false;
    /// FXAA 抗锯齿(默认关):场景经离屏 FBO + 全屏 FXAA 消除锯齿。与
    /// passthrough 同走离屏后处理通路,两者都开时 FXAA 优先。当前仅 GLES。
    bool fxaa = false;
    /// Aerial fog 大气光学深度雾(默认关,osgEarth 式):场景经离屏 FBO,采样
    /// 深度重建视距,沿视线积分指数密度剖面光学深度,远处/贴地平线地形自然
    /// 饱和混向天空色,近处山坡保持本色(无横切白带)。填补大气 pass 契约里
    /// "地表雾"的空缺。优先级高于 FXAA。当前仅 GLES。
    bool aerialFog = false;
    /// 雾消光系数(光学深度积分的 1/米系数)与起雾距离(米,积分从该距离开始)。
    /// 雾色由 shader 每像素从大气模型算(随视线/高度/太阳自然同调天空),不是
    /// 常数,故无雾色配置项;雾量由视线穿过的光学深度决定(无角度窗口)。
    /// 默认 3e-5:1500m 相机水平 138km 处雾≈96%,10km 山脊≈20%,2km 近景≈5%。
    float aerialFogDensity = 3.0e-5f;
    float aerialFogStartDistance = 0.0f;
    /// 日落地表着色强度(运行时可配)。太阳贴地平线时地形受光面转暖橙、阴影面
    /// 暖补光,解决"天空黄昏/地面正午"割裂。warmth=受光暖度(0=不变,1=最暖)、
    /// shadowWarmth=阴影暖补光缩放。色基准为引擎内「自然」档;白天(太阳高)自动
    /// 无效=零回归。默认=自然档。
    float sunsetTerrainWarmth = 0.75f;
    float sunsetTerrainShadowWarmth = 1.0f;
    /// 北极星 Phase 2b 虚拟纹理 C 方案 PoC(默认关,测量台专用):每帧跑
    /// feedback→回读→页表整链,量移动端固定开销(回读 stall),数报进 EarthPerf
    /// 头行 vtReadback=。不改任何渲染,纯旁路测量。当前两后端回读均已实现。
    bool virtualTexturePoc = false;
    /// 北极星 Phase 2b B 方案(逐瓦片合成)PoC(默认关,测量台专用):每帧对当前
    /// 可见瓦片数做 N 个离屏 bake pass,量 B 的每帧烘焙开销(vs C 的回读税),报进
    /// EarthPerf 头行 bBake=。纯旁路测量。
    bool tileCompositeBakePoc = false;
    /// 北极星 Phase 2b 合成方案「门①」原型(默认关,测量台专用):一屏 fill 跑
    /// baseline(1 次 atlas 采样)vs descent(N 次依赖间接 fetch + atlas 采样),
    /// 量逐片元间接采样倍率,报进 EarthPerf 头行 vtiRatio=。纯旁路测量。
    bool vtIndirectionSamplePoc = false;
    /// 北极星 合成方案「门③ Step3」页存储原型(默认关):建 texture2DArray 页存储
    /// 挂到一个 capped 真实地形瓦片,terrain 片元按页表 layer 采样。Step3a 合成
    /// 图案(隔离渲染路径),Step3b 换真实高清影像。生产渲染路径,非旁路测量台。
    bool terrainPageStore = false;
    /// GPU 逐区间计时(默认关,测量台专用):把一帧 GPU 时间线切成 pass/命令桶
    /// 若干段,每秒一行 `GpuPass` 进 logcat。开销 = 每段一个 timer query,静态
    /// 场景下 ~8 段/帧。仅 GLES(Metal 后端为 no-op)。判读边界见
    /// renderer/GpuFrameTiming.h。
    bool gpuPassTiming = false;

    /// 黑块探针:swap 前逐帧回读降采样帧统计近黑占比,超阈值逐帧告警
    /// (logcat `BlackProbe`)。截图/录屏抽样会漏帧,这是唯一不漏的像素级
    /// 判据。含同步回读(~1-2ms/帧),仅诊断会话开启。
    bool blackFrameProbe = false;
    /// 帧级按需渲染(默认关):画面与在途工作都收敛后停止排帧,宿主线程真正
    /// 睡下去,直到输入/异步产物把它唤醒。对齐 maplibre 的事件型+收敛型双
    /// 判据(见 Engine::setFrameGatingEnabled 的注释)。
    /// ⚠️ 失效方向是"画面冻住且零报错",接线新的异步产物时必须同步置脏位。
    bool frameGating = false;
    /// B:地形高度/法线纹理 GPU 烘焙(替代 CPU bakeTerrainHeightNormalTexels)。
    /// **默认开**:GLES 上已验证正确+无缝安全(f32 缝=CPU double 1.5°,
    /// test_terrain_bake_f32_seam 锁死)+更顺滑(dense 瓦片 descent 顿挫 4/min→0)+
    /// 热中性。后端守卫(TerrainDisplacementTemplatePool::acquireHeightTexture)
    /// 保证仅 OpenGLES 走 GPU 烘焙,Metal/Vulkan 恒回退 CPU 烘焙(正确)。
    /// ⚠️ MSL 版待有 Metal 设备验证后再补(见 TerrainHeightBakeShader.h);在那之前
    /// Metal/iOS 走 CPU 烘焙,dense descent 顿挫在 iOS 上保留。
    bool gpuHeightBake = true;
    /// 影子渲染自检(见 Engine::setShadowVerifyEnabled)。dev 专用,默认关,
    /// **严禁在性能测量时开启**(自检帧含同步回读 = 管线 flush)。
    bool shadowVerifyIdle = false;
};

} // namespace earth_engine
