#include "earth_engine/sdk/EarthSceneConfig.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace earth_engine;

TEST(EarthSceneConfig, DefaultsAreEmptyAndNonRendering) {
    EarthSceneConfig config;

    EXPECT_EQ(config.terrain.kind, TerrainSourceKind::None);
    EXPECT_TRUE(config.rasterOverlays.empty());
    EXPECT_TRUE(config.mvtSources.empty());
    EXPECT_FALSE(config.gltf.enabled);
    EXPECT_DOUBLE_EQ(config.fixedSimulationJulianDate, 0.0);
}

TEST(EarthSceneConfig, StoresMultipleMvtSourceDefinitions) {
    EarthSceneConfig config;
    MvtSourceConfig roads;
    roads.id = "roads";
    roads.urlTemplate = "https://a.example/{z}/{x}/{y}.pbf";
    roads.subdomains = {"a", "b", "c"};
    roads.minimumZoom = 3;
    roads.maximumZoom = 14;
    roads.includeLayers = {"roads"};
    roads.refinement = VectorTileRefinementPolicy::GeometryReplace;
    MvtSourceConfig pois;
    pois.id = "pois";
    pois.urlTemplate = "https://b.example/{z}/{x}/{y}.pbf";
    pois.includeLayers = {"poi"};
    config.mvtSources = {roads, pois};

    EarthSceneConfig copied = config;
    ASSERT_EQ(copied.mvtSources.size(), 2u);
    EXPECT_EQ(copied.mvtSources[0].id, "roads");
    EXPECT_EQ(copied.mvtSources[0].refinement,
              VectorTileRefinementPolicy::GeometryReplace);
    EXPECT_EQ(copied.mvtSources[0].subdomains,
              (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(copied.mvtSources[1].urlTemplate,
              "https://b.example/{z}/{x}/{y}.pbf");
}

TEST(EarthSceneConfig, StoresSceneSourceDefinitions) {
    EarthSceneConfig config;
    config.initialCamera = {106.508, 29.617, 30000.0};
    config.terrain = {
        TerrainSourceKind::Heightmap,
        "http://terrain.example/{z}/{x}/{y}.png",
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
    imagery.georeference =
        RasterOverlayGeoreference::Gcj02WebMercator;
    config.rasterOverlays.push_back(imagery);
    config.fixedSimulationJulianDate = 2461188.75;

    EarthSceneConfig copied = config;
    EXPECT_DOUBLE_EQ(copied.initialCamera.longitudeDegrees, 106.508);
    EXPECT_EQ(copied.terrain.kind, TerrainSourceKind::Heightmap);
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
    EXPECT_EQ(
        RasterOverlayGeoreference::Gcj02WebMercator,
        copied.rasterOverlays[0].georeference);
    EXPECT_DOUBLE_EQ(copied.fixedSimulationJulianDate, 2461188.75);
}

TEST(EarthSceneConfig, ExposesOnlyTilesetTerrainSourceKinds) {
    EXPECT_NE(TerrainSourceKind::None, TerrainSourceKind::Heightmap);
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

TEST(EarthSceneConfig, StoresGoogleMapTilesImagerySourceDefinitions) {
    EarthSceneConfig config;
    RasterOverlaySourceConfig overlay;
    overlay.imageryKind = ImagerySourceKind::GoogleMapTiles;
    overlay.googleMapTilesApiBaseUrl = "https://tile.googleapis.com/";
    overlay.googleMapTilesKey = "google-key";
    overlay.googleMapTilesSession = "session-token";
    overlay.googleMapTilesMapType = "roadmap";
    overlay.googleMapTilesLanguage = "zh-CN";
    overlay.googleMapTilesRegion = "CN";
    overlay.googleMapTilesImageFormat = "png";
    overlay.googleMapTilesScale = "scaleFactor2x";
    overlay.googleMapTilesHighDpi = true;
    overlay.googleMapTilesLayerTypes =
        std::vector<std::string>{"layerRoadmap", "layerTraffic"};
    overlay.googleMapTilesStyles = nlohmann::json::array(
        {nlohmann::json{{"featureType", "road"}}});
    overlay.googleMapTilesOverlay = false;
    overlay.attribution = "google attribution";
    overlay.maximumZoom = 28;
    overlay.imageryTileWidth = 512;
    overlay.imageryTileHeight = 256;
    config.rasterOverlays.push_back(overlay);

    ASSERT_EQ(1u, config.rasterOverlays.size());
    const RasterOverlaySourceConfig& copied = config.rasterOverlays[0];
    EXPECT_EQ(ImagerySourceKind::GoogleMapTiles, copied.imageryKind);
    EXPECT_EQ("https://tile.googleapis.com/",
              copied.googleMapTilesApiBaseUrl);
    EXPECT_EQ("google-key", copied.googleMapTilesKey);
    EXPECT_EQ("session-token", copied.googleMapTilesSession);
    EXPECT_EQ("roadmap", copied.googleMapTilesMapType);
    EXPECT_EQ("zh-CN", copied.googleMapTilesLanguage);
    EXPECT_EQ("CN", copied.googleMapTilesRegion);
    ASSERT_TRUE(copied.googleMapTilesImageFormat);
    EXPECT_EQ("png", *copied.googleMapTilesImageFormat);
    ASSERT_TRUE(copied.googleMapTilesScale);
    EXPECT_EQ("scaleFactor2x", *copied.googleMapTilesScale);
    ASSERT_TRUE(copied.googleMapTilesHighDpi);
    EXPECT_TRUE(*copied.googleMapTilesHighDpi);
    ASSERT_TRUE(copied.googleMapTilesLayerTypes);
    EXPECT_EQ((std::vector<std::string>{"layerRoadmap", "layerTraffic"}),
              *copied.googleMapTilesLayerTypes);
    ASSERT_TRUE(copied.googleMapTilesStyles);
    EXPECT_EQ("road", (*copied.googleMapTilesStyles)[0]["featureType"]);
    ASSERT_TRUE(copied.googleMapTilesOverlay);
    EXPECT_FALSE(*copied.googleMapTilesOverlay);
    EXPECT_EQ("google attribution", copied.attribution);
    EXPECT_EQ(28, copied.maximumZoom);
    EXPECT_EQ(512, copied.imageryTileWidth);
    EXPECT_EQ(256, copied.imageryTileHeight);
}
