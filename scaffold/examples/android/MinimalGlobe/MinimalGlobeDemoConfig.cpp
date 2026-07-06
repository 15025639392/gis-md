#include "MinimalGlobeDemoConfig.h"

namespace earth_engine::minimal_globe_demo {

EarthSceneConfig makeDefaultDemoSceneConfig() {
    EarthSceneConfig config;
    config.initialCamera = {
        kDemoCameraLongitudeDegrees,
        kDemoCameraLatitudeDegrees,
        kDemoCameraHeightMeters,
    };

    if (kEnableTerrainForDemo) {
        config.terrain.kind = TerrainSourceKind::QuantizedMesh;
        config.terrain.tileSize = 65;
        config.terrain.enableWaterMask = true;
        config.terrain.minimumZoom = 0;
        if (kUseCesiumIonTerrain) {
            // Cesium World Terrain：全球，运行时 ion endpoint 协商。
            config.terrain.attribution = "Cesium World Terrain";
            config.terrain.maximumZoom = 16;
            config.terrain.cesiumIonAssetId = kCesiumIonTerrainAssetId;
            config.terrain.cesiumIonAccessToken = kCesiumIonAccessToken;
        } else {
            // 本地 FABDEM（重庆，z0-12）。
            config.terrain.urlTemplate = kQuantizedMeshTerrainTemplate;
            config.terrain.layerJsonUrl = kQuantizedMeshTerrainLayerJson;
            config.terrain.attribution = "QuantizedMesh Terrain";
            config.terrain.maximumZoom = 12;
        }
    }
    config.tileset = {
        4.0,
        2.0,
    };
    // 运动期跳过快速划走的瓦片网络请求(cesium-js cullRequestsWhileMoving)。
    // 拖动/缩放中减少瞬时加载洪泛,相机停下恢复正常加载。
    config.tileset.cullRequestsWhileMoving = true;
    // NOTE: LOD-transition alpha cross-fade (enableLodTransitionPeriod) is
    // available via SceneTilesetConfig but left OFF here. The current built-in
    // cross-fade fades parent+child simultaneously, so mid-transition the black
    // clear-color bleeds through (~25% at midpoint) → visible dark block. Pop
    // looks better until the fade compositing is fixed to keep one opaque layer.

    if (kUseGaodeSatelliteForDemo) {
        RasterOverlaySourceConfig satellite;
        satellite.imageryKind = ImagerySourceKind::Xyz;
        satellite.urlTemplate = kGaodeSatelliteTemplate;
        satellite.attribution = "Gaode/Amap satellite";
        satellite.minimumZoom = 0;
        satellite.maximumZoom = 18;
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
    config.fixedSimulationJulianDate = kFixedSimulationJulianDate;
    return config;
}

} // namespace earth_engine::minimal_globe_demo
