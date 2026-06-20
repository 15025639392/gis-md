#include "earth_engine/sdk/EarthSceneConfig.h"

#include <gtest/gtest.h>

using namespace earth_engine;

TEST(EarthSceneConfig, DefaultsAreEmptyAndNonRendering) {
    EarthSceneConfig config;

    EXPECT_EQ(config.terrain.kind, TerrainSourceKind::None);
    EXPECT_TRUE(config.rasterOverlays.empty());
    EXPECT_FALSE(config.gltf.enabled);
    EXPECT_DOUBLE_EQ(config.fixedSimulationJulianDate, 0.0);
}

TEST(EarthSceneConfig, StoresSceneSourceDefinitions) {
    EarthSceneConfig config;
    config.initialCamera = {106.508, 29.617, 30000.0};
    config.terrain = {
        TerrainSourceKind::QuantizedMesh,
        "http://terrain.example/{z}/{x}/{y}.terrain",
        "http://terrain.example/layer.json",
        "terrain",
        0,
        12,
        65,
        false,
    };
    config.tileset = {4.0, 2.0};
    config.rasterOverlays.push_back({
        ImagerySourceKind::Xyz,
        "http://imagery.example/{z}/{x}/{y}.png",
        "",
        "imagery",
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
    config.fixedSimulationJulianDate = 2461188.75;

    EarthSceneConfig copied = config;
    EXPECT_DOUBLE_EQ(copied.initialCamera.longitudeDegrees, 106.508);
    EXPECT_EQ(copied.terrain.kind, TerrainSourceKind::QuantizedMesh);
    EXPECT_EQ(copied.terrain.maximumZoom, 12);
    EXPECT_DOUBLE_EQ(copied.tileset.mainThreadLoadingTimeLimit, 4.0);
    EXPECT_DOUBLE_EQ(copied.tileset.tileCacheUnloadTimeLimit, 2.0);
    ASSERT_EQ(copied.rasterOverlays.size(), 1u);
    EXPECT_EQ(copied.rasterOverlays[0].role,
              RasterOverlayRole::AnnotationOverlay);
    EXPECT_EQ(copied.rasterOverlays[0].tileMapResourceUrl, "");
    EXPECT_EQ(copied.rasterOverlays[0].fallbackPolicy,
              RasterOverlayFallbackPolicy::SkipUntilReady);
    EXPECT_FALSE(copied.rasterOverlays[0].blocksCompleteRenderable);
    EXPECT_DOUBLE_EQ(copied.fixedSimulationJulianDate, 2461188.75);
}

TEST(EarthSceneConfig, StoresTileMapServiceImagerySourceDefinitions) {
    EarthSceneConfig config;
    config.rasterOverlays.push_back({
        ImagerySourceKind::TileMapService,
        "",
        "https://example.com/tms/tilemapresource.xml",
        "tms attribution",
    });

    ASSERT_EQ(1u, config.rasterOverlays.size());
    EXPECT_EQ(ImagerySourceKind::TileMapService,
              config.rasterOverlays[0].imageryKind);
    EXPECT_EQ("https://example.com/tms/tilemapresource.xml",
              config.rasterOverlays[0].tileMapResourceUrl);
    EXPECT_EQ("tms attribution", config.rasterOverlays[0].attribution);
}
