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

    if (kUseGaodeSatelliteForDemo) {
        config.rasterOverlays.push_back({
            ImagerySourceKind::Xyz,
            kGaodeSatelliteTemplate,
            "Gaode/Amap satellite",
            0,
            18,
            0,
            0,
            20,
            2.0,
            1.0f,
            RasterOverlayRole::BaseImagery,
            RasterOverlayPriority::High,
            RasterOverlayFallbackPolicy::AncestorOrPlaceholder,
            true,
        });

        if (kEnableGaodeRoadNetOverlayForDemo) {
            config.rasterOverlays.push_back({
                ImagerySourceKind::Xyz,
                kGaodeRoadNetTemplate,
                "Gaode/Amap road network",
                0,
                18,
                0,
                0,
                20,
                2.0,
                0.92f,
                RasterOverlayRole::AnnotationOverlay,
                RasterOverlayPriority::Low,
                RasterOverlayFallbackPolicy::SkipUntilReady,
                false,
            });
        }
    } else {
        config.rasterOverlays.push_back({
            ImagerySourceKind::Debug,
            "",
            "",
            0,
            0,
            0,
            0,
            20,
            2.0,
            1.0f,
            RasterOverlayRole::BaseImagery,
            RasterOverlayPriority::High,
            RasterOverlayFallbackPolicy::AncestorOrPlaceholder,
            true,
        });
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
