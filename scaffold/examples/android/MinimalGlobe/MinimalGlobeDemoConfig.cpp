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
        config.terrain = {
            TerrainSourceKind::QuantizedMesh,
            kQuantizedMeshTerrainTemplate,
            kQuantizedMeshTerrainLayerJson,
            "QuantizedMesh Terrain",
            0,
            12,
            65,
            false,
        };
    }
    config.tileset = {
        4.0,
        2.0,
    };
    // Light up the built-in LOD-transition alpha cross-fade (cesium
    // enableLodTransitionPeriod) so tile refine/coarsen fades instead of
    // popping. Library default stays off (cesium-faithful); the demo turns it
    // on to showcase it. 0.5s is a short, responsive transition.
    config.tileset.enableLodTransitionPeriod = true;
    config.tileset.lodTransitionLength = 0.5f;

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
