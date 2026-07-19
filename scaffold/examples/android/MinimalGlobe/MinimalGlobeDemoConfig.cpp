#include "MinimalGlobeDemoConfig.h"

namespace earth_engine::minimal_globe_demo {

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
    // 测量台冻结相机：初始位姿设定后 update() 空转，far 位姿也可复现（见 header）。
    config.initialCamera.freezeCamera = kMeasureFreezeCamera;

    if (kEnableTerrainForDemo) {
        // 唯一地形源 = 规则栅格 raster-DEM 高度图（CPU 烘焙路径）。重庆 FABDEM
        // z0-12，65×65 顶点对齐 Mapbox Terrain-RGB PNG。无数据区回落椭球。
        // QuantizedMesh / Cesium ion 路径已退役。
        config.terrain.kind = TerrainSourceKind::Heightmap;
        config.terrain.urlTemplate = kHeightmapTerrainTemplate;
        config.terrain.heightmapEncoding =
            TerrainHeightmapEncoding::MapboxTerrainRgb;
        config.terrain.tileSize = 65;
        config.terrain.minimumZoom = 0;
        config.terrain.maximumZoom = 12;
        config.terrain.attribution = "FABDEM Terrain-RGB (grid65)";
        config.terrain.ellipsoidFallback = true;
        config.terrain.ellipsoidFallbackMaxZoom = 12;
    }
    config.tileset = {
        4.0,
        2.0,
    };
    // 北极星 Phase 2a 断纹理/几何耦合(flag 灰度,见 header)。
    config.tileset.decoupleImageryFromGeometry =
        kMeasureDecoupleImageryFromGeometry;
    // Android demo budget: keep visible detail unchanged, but do not retain the
    // desktop/cesium-native 512MB off-screen tile cache on a phone.
    config.tileset.maximumCachedBytes = 192LL * 1024 * 1024;
    // 运动期跳过快速划走的瓦片网络请求(cesium-js cullRequestsWhileMoving)。
    // 拖动/缩放中减少瞬时加载洪泛,相机停下恢复正常加载。
    config.tileset.cullRequestsWhileMoving = true;
    // 地形 fill 代理:根瓦片加载期间,先把已到的影像贴到椭球代理,真实地形网格
    // 到达后再替换,避免加载窗口只剩天空。
    config.tileset.enableTerrainFillProxy = true;
    // LOD geomorph:距离连续 geomorph 已启用(P2 引擎 + P3 skirt 之上)。morph 进度
    // 纯由本瓦片 SSE 驱动(finalizer,gate 在 maxSSE>0),**与时序 fade 计时器解耦**:
    // enableLodTransitionPeriod 保持 false=计时器关(否则每帧 fade discovery +
    // kick-keeps-fading 使运动期工作集膨胀=卡顿),morph 不需要它。子瓦片从
    // morph=0(coarse-self≈父面,worker 烘焙 heightDelta,规则栅格 in-tile 自降采样,
    // 根治旧变体 A「浮上来」)平滑 morph 到 morph=1(真实细节),随相机连续移动推进,
    // 消除硬 pop。相邻瓦片不同 morph 进度间的边缝由 skirt 遮盖。
    config.tileset.enableLodTransitionPeriod = false;
    config.tileset.lodTransitionLength = 1.0f;

    if (kUseGaodeSatelliteForDemo) {
        RasterOverlaySourceConfig satellite;
        satellite.imageryKind = ImagerySourceKind::Xyz;
        satellite.urlTemplate = kGaodeSatelliteTemplate;
        satellite.attribution = "Gaode/Amap satellite";
        satellite.minimumZoom = 0;
        satellite.maximumZoom = kMeasureImageryMaxZoom;
        satellite.overlayMinimumZoom = 0;
        satellite.overlayMaximumZoom = 0;
        satellite.maximumSimultaneousTileLoads = 20;
        satellite.maximumScreenSpaceError = 2.0;
        satellite.opacity = 1.0f;
        satellite.role = RasterOverlayRole::BaseImagery;
        satellite.priority = RasterOverlayPriority::High;
        satellite.fallbackPolicy = RasterOverlayFallbackPolicy::AncestorOrPlaceholder;
        satellite.blocksCompleteRenderable = true;
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
    // 北极星 合成方案 门③ Step3 页存储原型(默认关,见 header)。
    config.terrainPageStore = kEnableTerrainPageStore;
    config.fixedSimulationJulianDate = kFixedSimulationJulianDate;
    if (kEnableInstancedI3dmDemo) {
        // 默认固定时间是宾州凌晨(树在夜侧无光照=全黑),实例化观察改用当地白天
        // (~17:00 UTC = 宾州正午)让树被太阳照亮可见。
        config.fixedSimulationJulianDate = kFixedSimulationJulianDate + 11.0 / 24.0;
    }
    return config;
}

} // namespace earth_engine::minimal_globe_demo
