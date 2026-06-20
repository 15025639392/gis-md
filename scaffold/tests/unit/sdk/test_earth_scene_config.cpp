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
    RasterOverlaySourceConfig imagery;
    imagery.imageryKind = ImagerySourceKind::Xyz;
    imagery.urlTemplate = "http://imagery.example/{z}/{x}/{y}.png";
    imagery.attribution = "imagery";
    imagery.maximumZoom = 18;
    imagery.opacity = 0.92f;
    imagery.role = RasterOverlayRole::AnnotationOverlay;
    imagery.priority = RasterOverlayPriority::Low;
    imagery.fallbackPolicy = RasterOverlayFallbackPolicy::SkipUntilReady;
    imagery.blocksCompleteRenderable = false;
    config.rasterOverlays.push_back(imagery);
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
    RasterOverlaySourceConfig overlay;
    overlay.imageryKind = ImagerySourceKind::TileMapService;
    overlay.tileMapResourceUrl =
        "https://example.com/tms/tilemapresource.xml";
    overlay.attribution = "tms attribution";
    config.rasterOverlays.push_back(overlay);

    ASSERT_EQ(1u, config.rasterOverlays.size());
    EXPECT_EQ(ImagerySourceKind::TileMapService,
              config.rasterOverlays[0].imageryKind);
    EXPECT_EQ("https://example.com/tms/tilemapresource.xml",
              config.rasterOverlays[0].tileMapResourceUrl);
    EXPECT_EQ("tms attribution", config.rasterOverlays[0].attribution);
}

TEST(EarthSceneConfig, StoresWebMapServiceImagerySourceDefinitions) {
    EarthSceneConfig config;
    RasterOverlaySourceConfig overlay;
    overlay.imageryKind = ImagerySourceKind::WebMapService;
    overlay.urlTemplate = "https://example.com/wms";
    overlay.attribution = "wms attribution";
    overlay.minimumZoom = 1;
    overlay.maximumZoom = 12;
    overlay.wmsVersion = "1.1.1";
    overlay.wmsLayers = "imagery,labels";
    overlay.wmsFormat = "image/jpeg";
    overlay.imageryTileWidth = 512;
    overlay.imageryTileHeight = 256;
    config.rasterOverlays.push_back(overlay);

    ASSERT_EQ(1u, config.rasterOverlays.size());
    const RasterOverlaySourceConfig& copied = config.rasterOverlays[0];
    EXPECT_EQ(ImagerySourceKind::WebMapService, copied.imageryKind);
    EXPECT_EQ("https://example.com/wms", copied.urlTemplate);
    EXPECT_EQ("wms attribution", copied.attribution);
    EXPECT_EQ(1, copied.minimumZoom);
    EXPECT_EQ(12, copied.maximumZoom);
    EXPECT_EQ("1.1.1", copied.wmsVersion);
    EXPECT_EQ("imagery,labels", copied.wmsLayers);
    EXPECT_EQ("image/jpeg", copied.wmsFormat);
    EXPECT_EQ(512, copied.imageryTileWidth);
    EXPECT_EQ(256, copied.imageryTileHeight);
}

TEST(EarthSceneConfig, StoresWebMapTileServiceImagerySourceDefinitions) {
    EarthSceneConfig config;
    RasterOverlaySourceConfig overlay;
    overlay.imageryKind = ImagerySourceKind::WebMapTileService;
    overlay.urlTemplate = "https://example.com/wmts";
    overlay.attribution = "wmts attribution";
    overlay.minimumZoom = 2;
    overlay.maximumZoom = 10;
    overlay.wmtsFormat = "image/png";
    overlay.wmtsLayer = "imagery";
    overlay.wmtsStyle = "default";
    overlay.wmtsTileMatrixSetId = "GoogleMapsCompatible";
    overlay.wmtsSchemeId = "Geographic-TMS";
    overlay.wmtsTileMatrixLabels = {"0", "1", "2"};
    overlay.wmtsSubdomains = {"a", "b"};
    overlay.wmtsDimensions = {{"time", "2026-06-21"}};
    overlay.imageryTileWidth = 512;
    overlay.imageryTileHeight = 512;
    config.rasterOverlays.push_back(overlay);

    ASSERT_EQ(1u, config.rasterOverlays.size());
    const RasterOverlaySourceConfig& copied = config.rasterOverlays[0];
    EXPECT_EQ(ImagerySourceKind::WebMapTileService, copied.imageryKind);
    EXPECT_EQ("https://example.com/wmts", copied.urlTemplate);
    EXPECT_EQ("wmts attribution", copied.attribution);
    EXPECT_EQ(2, copied.minimumZoom);
    EXPECT_EQ(10, copied.maximumZoom);
    EXPECT_EQ("image/png", copied.wmtsFormat);
    EXPECT_EQ("imagery", copied.wmtsLayer);
    EXPECT_EQ("default", copied.wmtsStyle);
    EXPECT_EQ("GoogleMapsCompatible", copied.wmtsTileMatrixSetId);
    EXPECT_EQ("Geographic-TMS", copied.wmtsSchemeId);
    EXPECT_EQ((std::vector<std::string>{"0", "1", "2"}),
              copied.wmtsTileMatrixLabels);
    EXPECT_EQ((std::vector<std::string>{"a", "b"}),
              copied.wmtsSubdomains);
    EXPECT_EQ("2026-06-21", copied.wmtsDimensions.at("time"));
    EXPECT_EQ(512, copied.imageryTileWidth);
    EXPECT_EQ(512, copied.imageryTileHeight);
}

TEST(EarthSceneConfig, StoresBingMapsImagerySourceDefinitions) {
    EarthSceneConfig config;
    RasterOverlaySourceConfig overlay;
    overlay.imageryKind = ImagerySourceKind::BingMaps;
    overlay.bingBaseUrl = "https://dev.virtualearth.net/";
    overlay.bingMapStyle = "Road";
    overlay.bingKey = "bing-key";
    overlay.urlTemplate =
        "https://ecn.{subdomain}.tiles.virtualearth.net/tiles/a{quadkey}.jpeg?mkt={culture}";
    overlay.attribution = "bing attribution";
    overlay.minimumZoom = 1;
    overlay.maximumZoom = 19;
    overlay.imageryTileWidth = 256;
    overlay.imageryTileHeight = 256;
    overlay.bingCulture = "en-US";
    overlay.bingSubdomains = {"t0", "t1", "t2"};
    config.rasterOverlays.push_back(overlay);

    ASSERT_EQ(1u, config.rasterOverlays.size());
    const RasterOverlaySourceConfig& copied = config.rasterOverlays[0];
    EXPECT_EQ(ImagerySourceKind::BingMaps, copied.imageryKind);
    EXPECT_EQ("https://dev.virtualearth.net/", copied.bingBaseUrl);
    EXPECT_EQ("Road", copied.bingMapStyle);
    EXPECT_EQ("bing-key", copied.bingKey);
    EXPECT_EQ(
        "https://ecn.{subdomain}.tiles.virtualearth.net/tiles/a{quadkey}.jpeg?mkt={culture}",
        copied.urlTemplate);
    EXPECT_EQ("bing attribution", copied.attribution);
    EXPECT_EQ(1, copied.minimumZoom);
    EXPECT_EQ(19, copied.maximumZoom);
    EXPECT_EQ(256, copied.imageryTileWidth);
    EXPECT_EQ(256, copied.imageryTileHeight);
    EXPECT_EQ("en-US", copied.bingCulture);
    EXPECT_EQ((std::vector<std::string>{"t0", "t1", "t2"}),
              copied.bingSubdomains);
}
