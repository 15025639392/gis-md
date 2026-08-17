#include "MinimalGlobeDemoConfig.h"

#include "earth_engine/data/StyleFilter.h"

namespace earth_engine::minimal_globe_demo {

VectorRasterStyle makeMvtDrapeStyle() {
    // 刀1:只配**面**层(水/建筑色块)。线(roads)不进 drape —— 线对
    // 栅格模糊极敏感(E4 死因),留几何通路守擂,刀2 换 SDF 场。
    // 颜色与几何通路 GLESView 的 fillColorExpr 对齐(0-255 = float×255):
    // water (0.25,0.50,0.85,0.55)、building (0.60,0.60,0.62,0.55) ——
    // 交接时观感不变;alpha 沿用半透明,叠在卫星影像上不糊死地物。
    VectorRasterStyle style;
    // 背景全透明:底图矢量叠在卫星影像之上,不该盖住影像。
    style.background = {0, 0, 0, 0};

    VectorRasterLayerPaint water;
    water.layer = "water";
    water.fillColor = {64, 128, 217, 140};

    VectorRasterLayerPaint building;
    building.layer = "building";
    // 建筑 zoom+距离分级。距离经**页 zoom**(屏幕清晰度)隐含:远景=低 z 页、
    // 近景=高 z 页,故按页 z 分档即按相机距离分档。数据只带 `building`(OSM
    // 类型)与可选 `name` —— 无 render_height/area/levels,无法按物理体量分级,
    // 改以"是否地标"代理体量,两档:
    //   • z13-15(中距):仅**命名建筑** 或 **地标类型**(商业/医院/学校/场馆…)
    //     —— 远景只留可辨认的大体量,generic 民房(building=yes/house)不在此距
    //     糊成灰斑;
    //   • z>=15(近景):全部建筑铺满。
    // minZoom=13 仍是地板(更远整层不烘,零逐要素成本)。styleZoom 可到 z17+,
    // 数据钳在 z14 不影响这些门槛(门槛比的是页 z,非数据 z)。
    building.minZoom = 13;
    building.filter = StyleFilter::any({
        StyleFilter::has("name"),
        StyleFilter::in("building",
                        {"commercial", "industrial", "retail", "warehouse",
                         "hospital", "school", "university", "college",
                         "stadium", "hotel", "apartments", "church", "temple",
                         "mosque", "public", "civic", "government",
                         "train_station", "hangar"}),
        StyleFilter::zoomCompare(StyleFilter::Compare::GreaterEqual, 15),
    });
    building.fillColor = {153, 153, 158, 140};

    // 顺序 = 绘制顺序:水面在下,建筑在上。
    style.layers = {water, building};
    return style;
}

VectorRasterStyle makeMvtDrapeStyleNight() {
    // V26 一期换肤验证:日版改色,分级/filter 原样保留(唯一权威在日版)。
    // 水=深墨蓝、建筑=暖棕,与日版色相差开到肉眼即判;alpha 略提,夜景
    // 底图偏暗、盖度可以更高。
    VectorRasterStyle style = makeMvtDrapeStyle();
    for (VectorRasterLayerPaint& paint : style.layers) {
        if (paint.layer == "water") {
            paint.fillColor = {18, 42, 92, 170};
        } else if (paint.layer == "building") {
            paint.fillColor = {120, 92, 60, 170};
        }
    }
    return style;
}

VectorRasterStyle makeMvtRoadFieldStyle() {
    using C = StyleFilter::Compare;
    // highway 分级:与几何通路退役前的分级表同源(zoom 固定于**页 z**,
    // styleZoom 跟屏幕清晰度走,可到 z17+)。粗档只留干线,细档逐步放开 ——
    // 低 zoom 页 texel 覆盖地面大,道路间距小于线宽会连成白块。
    StyleFilter::Ptr grading = StyleFilter::any({
        StyleFilter::all({
            StyleFilter::zoomCompare(C::Less, 10),
            StyleFilter::in("highway", {"motorway", "trunk"})}),
        StyleFilter::all({
            StyleFilter::zoomCompare(C::GreaterEqual, 10),
            StyleFilter::zoomCompare(C::Less, 12),
            StyleFilter::in("highway", {"motorway", "trunk", "primary"})}),
        StyleFilter::all({
            StyleFilter::zoomCompare(C::GreaterEqual, 12),
            StyleFilter::zoomCompare(C::Less, 13),
            StyleFilter::in("highway", {"motorway", "trunk", "primary",
                                        "secondary"})}),
        StyleFilter::all({
            StyleFilter::zoomCompare(C::GreaterEqual, 13),
            StyleFilter::zoomCompare(C::Less, 14),
            StyleFilter::in("highway", {"motorway", "trunk", "primary",
                                        "secondary", "tertiary"})}),
        // 次级路细分:z>=14 的"全收"catch-all 拆成两段距离档,让居民/支路按
        // 相机距离分级出现,而非在 z14 一次性全糊上来。
        //   • z14-15(中近):+ residential / unclassified / living_street ——
        //     成街的次级路网先补齐;
        //   • z>=15(贴近):再放开 service / track / path / footway / … 等
        //     最末梢(catch-all,无 highway 约束 = 剩余全收)。
        StyleFilter::all({
            StyleFilter::zoomCompare(C::GreaterEqual, 14),
            StyleFilter::zoomCompare(C::Less, 15),
            StyleFilter::in("highway", {"motorway", "trunk", "primary",
                                        "secondary", "tertiary", "residential",
                                        "unclassified", "living_street"})}),
        StyleFilter::zoomCompare(C::GreaterEqual, 15),
    });

    // 宽度不再由样式档位承载(场编码只有中心线距离):线宽走 FS 分级
    // ramp(kMvtRoadFieldWidthRampPx,局部 zoom 连续插值,旧三档台阶的
    // 连续化)。本样式只剩「哪些路在哪个 zoom 参与烘焙」的 filter 职责。
    VectorRasterLayerPaint roads;
    roads.layer = "roads";
    roads.filter = grading;
    // lineColor 只用作"该层参与场烘焙"的开关(alpha>0);真实线色走
    // FS uniform(kMvtRoadFieldColor)。
    roads.lineColor = {255, 255, 255, 255};

    VectorRasterStyle style;
    style.layers = {roads};
    return style;
}

EarthSceneConfig makeDefaultDemoSceneConfig() {
    EarthSceneConfig config;
    if (kEnableInstancedI3dmDemo) {
        // 实例化基线:相机对准 tree.i3dm 真实位置(宾州),斜视(仰角 28°)俯瞰树阵,
        // 让竖直广告牌树叶可见(nadir 下隐形)。heightMeters=眼睛离地高度。
        config.initialCamera = {
            kTreeI3dmLongitudeDegrees,
            kTreeI3dmLatitudeDegrees,
            kTreeI3dmCameraHeightMeters,
            28.0,
        };
    } else {
        // 北极星 Phase 0 测量台:编译期钉死相机(见 header kMeasure*)。
        // kMeasureHorizonView 切地平线宽视野(低仰角高空),量 capped 瓦片数(B vs C)。
        config.initialCamera = {
            kMeasureLongitudeDegrees,
            kMeasureLatitudeDegrees,
            kMeasureHorizonView ? kMeasureHorizonHeightMeters
                                : kMeasureHeightMeters,
            kMeasureHorizonView ? kMeasureHorizonElevationDegrees
                                : kMeasureObliqueElevationDegrees,
        };
    }
    // 地平线日落 A/B:相机看向日落太阳方位(否则默认 0=正北,与旧行为等价)。
    config.initialCamera.obliqueAzimuthDegrees =
        (kMeasureHorizonView && kMeasureHorizonSunset)
            ? kMeasureHorizonAzimuthDegrees
            : 0.0;
    // 测量台冻结相机：初始位姿设定后 update() 空转，far 位姿也可复现（见 header）。
    config.initialCamera.freezeCamera = kMeasureFreezeCamera;
    // 测量台脚本化平移（§14.1② live 换页净测，见 header）：确定性受控运动量 ghost。
    config.initialCamera.scriptedPan = kMeasureScriptedPan;
    config.initialCamera.scriptedPanStartFrame = kMeasureScriptedPanStartFrame;
    config.initialCamera.scriptedPanFrames = kMeasureScriptedPanFrames;
    config.initialCamera.scriptedPanYawPerFrameRad =
        kMeasureScriptedPanYawPerFrameDegrees * 0.017453292519943295;  // deg→rad

    if (kEnableTerrainForDemo) {
        // 唯一地形源 = 规则栅格 raster-DEM 高度图（CPU 烘焙路径）。重庆 FABDEM
        // z0-12，65×65 顶点对齐 Mapbox Terrain-RGB PNG。无数据区回落椭球。
        // QuantizedMesh / Cesium ion 路径已退役。
        config.terrain.kind = TerrainSourceKind::Heightmap;
        config.terrain.heightmapEncoding =
            TerrainHeightmapEncoding::MapboxTerrainRgb;
        config.terrain.ellipsoidFallback = true;
        if (kUseGlobalTerrainSource) {
            // 全球 NASA/Mapbox 514×514 Terrain-RGB,z6-12。cell-registered +
            // 1px 重叠环 → borderInset=0.5(半像素内缩,相邻瓦片无缝,已单测
            // 锁定)。z0-5 无数据由 ellipsoidFallback 兜底(回落椭球面贴影像)。
            config.terrain.urlTemplate = kGlobalTerrainTemplate;
            config.terrain.tileSize = 514;
            config.terrain.minimumZoom = 6;
            config.terrain.maximumZoom = 12;
            config.terrain.heightmapMaxNativeZoom = 12;
            config.terrain.heightmapBorderInset = 0.5f;
            config.terrain.attribution = "NASA/Mapbox Terrain-RGB (514 global)";
            config.terrain.ellipsoidFallbackMaxZoom = 12;
        } else {
            // 本地自产 FABDEM raster-DEM,grid65 顶点栅格,重庆局部,z0-12。
            config.terrain.urlTemplate = kHeightmapTerrainTemplate;
            config.terrain.tileSize = 65;
            config.terrain.minimumZoom = 0;
            config.terrain.maximumZoom = 12;
            config.terrain.heightmapBorderInset = 0.0f;
            config.terrain.attribution = "FABDEM Terrain-RGB (grid65)";
            config.terrain.ellipsoidFallbackMaxZoom = 12;
        }
    }
    config.tileset = {
        4.0,
        2.0,
    };
    // 北极星生产主路径:纹理/几何解耦默认开(几何 cap 在 DEM native z12,影像不再
    // 驱动几何 refine → 无捏造 z13+ notReady 空洞 = 收底部露天空)。配下方
    // terrainPageStore 在 capped z12 面贴 z14+ 屏幕界定高清影像(近景仍 crisp)。
    // 二者是生产配对(§15.3⑤)。A/B 测耦合基线时把此处改 false。
    config.tileset.decoupleImageryFromGeometry = true;
    // Android demo budget: keep visible detail unchanged, but do not retain the
    // desktop/cesium-native 512MB off-screen tile cache on a phone.
    config.tileset.maximumCachedBytes = 192LL * 1024 * 1024;
    // 运动期跳过快速划走的瓦片网络请求(cesium-js cullRequestsWhileMoving)。
    // 拖动/缩放中减少瞬时加载洪泛,相机停下恢复正常加载。
    config.tileset.cullRequestsWhileMoving = true;
    // 地形 fill 代理:根瓦片加载期间,先把已到的影像贴到椭球代理,真实地形网格
    // 到达后再替换,避免加载窗口只剩天空。
    config.tileset.enableTerrainFillProxy = true;
    // 接边错位诊断探针默认关(常开每帧 ~4ms=selPlan 大头,无缝已收官)。
    config.tileset.seamEdgeMismatchProbe = kEnableSeamEdgeMismatchProbe;
    // LOD geomorph:距离连续 geomorph 已启用(P2 引擎 + P3 skirt 之上)。morph 进度
    // 纯由本瓦片 SSE 驱动(finalizer,gate 在 maxSSE>0)。子瓦片从 morph=0
    // (coarse-self≈父面,worker 烘焙 heightDelta,规则栅格 in-tile 自降采样,根治旧
    // 变体 A「浮上来」)平滑 morph 到 morph=1(真实细节),随相机连续移动推进,消除
    // 硬 pop。相邻瓦片不同 morph 进度间的边缝由 skirt 遮盖。时序 cross-fade 通路
    // 已于 2026-08-07 整链删除(被 geomorph 顶替)。

    if (kUseGaodeSatelliteForDemo) {
        RasterOverlaySourceConfig satellite;
        satellite.imageryKind = ImagerySourceKind::Xyz;
        satellite.urlTemplate = kGaodeSatelliteTemplate;
        satellite.attribution = "Gaode/Amap satellite";
        satellite.minimumZoom = 0;
        satellite.maximumZoom = kMeasureImageryMaxZoom;
        // Gaode 卫星 z0(整幅世界瓦片)是「无卫星图」橄榄灰占位图,z1 起才是真实
        // 影像。overlayMinimumZoom=1 让映射永不请求 z0 占位图 → 全球尺度粗瓦片
        // 直接贴 z1+ 真实影像(不再露橄榄灰)。
        satellite.overlayMinimumZoom = 1;
        satellite.overlayMaximumZoom = 0;
        satellite.maximumSimultaneousTileLoads = 20;
        satellite.maximumScreenSpaceError = 2.0;
        satellite.opacity = 1.0f;
        satellite.role = RasterOverlayRole::BaseImagery;
        satellite.priority = RasterOverlayPriority::High;
        satellite.fallbackPolicy = RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
        satellite.blocksCompleteRenderable = true;
        satellite.georeference =
            RasterOverlayGeoreference::Gcj02WebMercator;
        config.rasterOverlays.push_back(satellite);

        if (kEnableGaodeRoadNetOverlayForDemo) {
            RasterOverlaySourceConfig roadNet;
            roadNet.imageryKind = ImagerySourceKind::Xyz;
            roadNet.urlTemplate = kGaodeRoadNetTemplate;
            roadNet.attribution = "Gaode/Amap road network";
            roadNet.minimumZoom = 0;
            roadNet.maximumZoom = 18;
            roadNet.overlayMinimumZoom = 0;
            roadNet.overlayMaximumZoom = 0;
            roadNet.maximumSimultaneousTileLoads = 20;
            roadNet.maximumScreenSpaceError = 2.0;
            roadNet.opacity = 0.92f;
            roadNet.role = RasterOverlayRole::AnnotationOverlay;
            roadNet.priority = RasterOverlayPriority::Low;
            roadNet.fallbackPolicy = RasterOverlayFallbackPolicy::SkipUntilReady;
            roadNet.blocksCompleteRenderable = false;
            roadNet.georeference =
                RasterOverlayGeoreference::Gcj02WebMercator;
            config.rasterOverlays.push_back(roadNet);
        }
    } else {
        RasterOverlaySourceConfig debug;
        debug.imageryKind = ImagerySourceKind::Debug;
        debug.maximumSimultaneousTileLoads = 20;
        debug.maximumScreenSpaceError = 2.0;
        debug.opacity = 1.0f;
        debug.role = RasterOverlayRole::BaseImagery;
        debug.priority = RasterOverlayPriority::High;
        debug.fallbackPolicy = RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
        debug.blocksCompleteRenderable = false;  // 允许地形立即渲染
        config.rasterOverlays.push_back(debug);
    }

    if (kEnableInstancedI3dmDemo) {
        // 实例化基线:tree.i3dm 是世界锚定(绝对 ECEF, 宾州 lon-75.612/lat40.043,
        // 25 实例簇径~180m)。worldAnchored=true 跳过 ENU 就地放置,让树落回真实
        // 位置;相机在 §makeDefaultDemoSceneConfig 顶部改指该点低空(见 initialCamera)。
        config.gltf = {
            true,
            "Geographic-TMS",
            0,
            0,  // 西半球根(宾州 lon-75); 东半球是 x=1
            0,
            kTreeI3dmUrl,
            "Cesium tree.i3dm (instanced)",
            kTreeI3dmLongitudeDegrees,
            kTreeI3dmLatitudeDegrees,
            0.0,
            1.0,
            /*worldAnchored=*/true,
        };
    } else {
        config.gltf = {
            kEnableRobotExpressiveGltfDemo,
            "Geographic-TMS",
            0,
            1,
            0,
            kRobotExpressiveGlbUrl,
            "RobotExpressive GLB",
            kDemoCameraLongitudeDegrees,
            kDemoCameraLatitudeDegrees,
            650.0,
            420.0,
        };
    }
    // Aerial fog 距离雾:默认开,远处地形融进地平线霞(斜视地平线最明显,
    // nadir 下淡)。雾色/密度用 SDK 默认(亮地平线霞 + 6e-6 密度)。
    config.aerialFog = true;
    // 北极星 Phase 2b 虚拟纹理 PoC(默认关,见 header)。
    config.virtualTexturePoc = kMeasureVirtualTexturePoC;
    // 北极星 Phase 2b B 方案逐瓦片合成 PoC(默认关,见 header)。
    config.tileCompositeBakePoc = kMeasureTileCompositeBakePoC;
    // 北极星 Phase 2b 合成方案 门① 原型(默认关,见 header)。
    config.vtIndirectionSamplePoc = kMeasureVtIndirectionSamplePoC;
    // 北极星生产主路径:SVT 页存储默认开(配上方 decouple)。稀疏页存储在 capped
    // z12 地形面上贴屏幕界定 z14-17 高清影像(§15 Step B)。A/B 测时改 false。
    config.terrainPageStore = true;
    // GPU 逐区间计时(见 header)。默认关:每帧 ~8 个 timer query,是测量台不是
    // 生产开销。要查 GPU 时间去哪了就把 header 里那个常量翻 true。
    config.gpuPassTiming = kMeasureGpuPassTiming;
    config.blackFrameProbe = kBlackFrameProbe;
    config.frameGating = kEnableFrameGating;
    config.gpuHeightBake = kEnableGpuHeightBake;
    config.shadowVerifyIdle = kShadowVerifyIdle;
    config.fixedSimulationJulianDate = kFixedSimulationJulianDate;
    if (kMeasureHorizonView && kMeasureHorizonSunset) {
        // 日落 A/B:时钟设到太阳贴地平线的低角(覆盖地形验收的近顶太阳)。
        config.fixedSimulationJulianDate =
            2461188.75 + kMeasureHorizonSunsetJulianOffset;
    }
    if (kEnableInstancedI3dmDemo) {
        // 默认固定时间是宾州凌晨(树在夜侧无光照=全黑),实例化观察改用当地白天
        // (~17:00 UTC = 宾州正午)让树被太阳照亮可见。
        config.fixedSimulationJulianDate = kFixedSimulationJulianDate + 11.0 / 24.0;
    }
    return config;
}

} // namespace earth_engine::minimal_globe_demo
