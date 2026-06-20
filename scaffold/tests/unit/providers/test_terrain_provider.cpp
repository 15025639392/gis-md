#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/providers/HeightmapTerrainProvider.h"
#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/providers/TileMapServiceImageryProvider.h"
#include "earth_engine/providers/TileMapServiceUrl.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

using namespace earth_engine;

namespace {

class MockImagePlatformBridge : public PlatformBridge {
public:
    explicit MockImagePlatformBridge(std::vector<uint8_t> pixels)
        : pixels_(std::move(pixels)) {}

    void onMemoryPressure() override {}
    void onEnterBackground() override {}
    void onEnterForeground() override {}

    std::unique_ptr<HttpRequest> get(
        const std::string&,
        std::function<void(int, std::vector<uint8_t>)>,
        HttpRequestOptions = {}) override {
        return nullptr;
    }

    std::string cacheDirectory() const override { return {}; }
    std::string documentsDirectory() const override { return {}; }

    std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t*,
        size_t) override {
        auto image = std::make_unique<DecodedImage>();
        image->width = 2;
        image->height = 2;
        image->channels = 3;
        image->pixels = pixels_;
        return image;
    }

    void log(LogLevel, const std::string&, const std::string&) override {}
    DeviceInfo deviceInfo() const override { return {}; }
    std::string getToken(const std::string&) const override { return {}; }

private:
    std::vector<uint8_t> pixels_;
};

std::unique_ptr<DecodedHeightmap> makeFlatHeightmapForProviderTest(
    float height) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {height, height, height, height};
    heightmap->minHeight = height;
    heightmap->maxHeight = height;
    return heightmap;
}

} // namespace

// ============================================================
// URL 构建
// ============================================================

TEST(HeightmapTerrainProviderTest, BuildUrl) {
    HeightmapTerrainProvider provider(
        "https://tiles.example.com/terrarium/{z}/{x}/{y}.png");

    TileKey key{"XYZ-WebMercator", 10, 512, 256};
    std::string url = provider.buildUrl(key);

    EXPECT_EQ("https://tiles.example.com/terrarium/10/512/256.png", url);
}

TEST(HeightmapTerrainProviderTest, SchemeId) {
    HeightmapTerrainProvider provider("https://example.com/{z}/{x}/{y}.png");
    EXPECT_EQ("XYZ-WebMercator", provider.schemeId());
    EXPECT_EQ("heightmap-terrain", provider.type());
}

TEST(HeightmapTerrainProviderTest, ZoomRange) {
    HeightmapTerrainProvider provider("https://example.com/{z}/{x}/{y}.png");
    EXPECT_EQ(0, provider.minZoom());
    EXPECT_EQ(14, provider.maxZoom());
    EXPECT_EQ(256, provider.tileSize());

    provider.setZoomRange(4, 10);
    EXPECT_EQ(4, provider.minZoom());
    EXPECT_EQ(10, provider.maxZoom());
}

TEST(QuantizedMeshTerrainProviderTest, ConfiguresFromCesiumLayerJson) {
    QuantizedMeshTerrainProvider provider("https://example.com/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "tilejson": "2.1.0",
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 12,
      "available": [[{"startX":0,"startY":0,"endX":1,"endY":0}]]
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(layerJson, "http://192.168.1.8:8092/layer.json"));
    EXPECT_EQ(0, provider.minZoom());
    EXPECT_EQ(12, provider.maxZoom());
    EXPECT_EQ("http://192.168.1.8:8092/{z}/{x}/{y}.terrain", provider.urlTemplate());
    EXPECT_EQ("http://192.168.1.8:8092/12/6487/2685.terrain",
              provider.buildUrl(TileKey{"Geographic-TMS", 12, 6487, 2685}));
    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 1, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 12, 6487, 2685}));
}

TEST(QuantizedMeshTerrainProviderTest, WebMercatorLayerJsonUsesOneByOneRootLikeCesiumNative) {
    QuantizedMeshTerrainProvider provider(
        "https://example.com/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:3857",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "maxzoom": 12
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/terrain/layer.json"));

    EXPECT_EQ("XYZ-WebMercator", provider.schemeId());
    EXPECT_EQ("https://example.invalid/terrain/{z}/{x}/{y}.terrain",
              provider.urlTemplate());
    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 1, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}));
}

TEST(QuantizedMeshTerrainProviderTest, WebMercatorMetadataAvailabilityStartsAtOneRootLikeCesiumNative) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:3857",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "maxzoom": 10,
      "metadataAvailability": 2
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));

    EXPECT_EQ("XYZ-WebMercator", provider.schemeId());
    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 1, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 0, 1}));
    EXPECT_EQ(TileAvailabilityState::Unknown,
              provider.availabilityState(TileKey{"XYZ-WebMercator", 1, 0, 0}));
}

TEST(QuantizedMeshTerrainProviderTest, AvailabilityUsesInclusiveTileCenterLikeCesiumNative) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/{z}/{x}/{y}.terrain");
    provider.setZoomRange(0, 10);

    provider.addAvailabilityRects(2, {{{0, 0, 0, 0}}});

    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 1, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 1, 1, 0}));

    provider.addAvailabilityRects(2, {{{4, 0, 4, 0}}});
    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 1, 2, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 1, 3, 0}));
}

TEST(QuantizedMeshTerrainProviderTest, MetadataAvailabilityStartsUnknownChildrenLikeCesiumNative) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 17,
      "metadataAvailability": 10
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));

    const TileKey root{"Geographic-TMS", 0, 0, 0};
    const TileKey child{"Geographic-TMS", 1, 0, 0};

    EXPECT_TRUE(provider.supportsTile(root));
    EXPECT_EQ(TileAvailabilityState::Unknown, provider.availabilityState(child));
    EXPECT_FALSE(provider.supportsTile(child));

    provider.markSubtreeLoaded(0, 0);

    EXPECT_EQ(TileAvailabilityState::NotAvailable,
              provider.availabilityState(child));
}

TEST(QuantizedMeshTerrainProviderTest, MetadataAvailabilityLoadedSubtreeTableUsesCeilLikeCesiumNative) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "maxzoom": 20,
      "metadataAvailability": 10
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));

    EXPECT_FALSE(provider.isSubtreeLoaded(1, 0));
    EXPECT_TRUE(provider.isSubtreeLoaded(2, 0));
}

TEST(QuantizedMeshTerrainProviderTest, NonInt32MetadataAvailabilityIsIgnoredLikeCesiumNative) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "maxzoom": 4,
      "metadataAvailability": 2147483648,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":0,"startY":0,"endX":0,"endY":0}]
      ]
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));

    EXPECT_EQ(-1, provider.availabilityLevels());
    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 1, 0, 0}));
}

TEST(QuantizedMeshTerrainProviderTest, ZeroMetadataAvailabilityShadowsLayerAvailabilityLikeCesiumNative) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "maxzoom": 4,
      "metadataAvailability": 0,
      "available": [
        [{"startX":0,"startY":0,"endX":1,"endY":0}],
        [{"startX":0,"startY":0,"endX":0,"endY":0}]
      ]
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));

    EXPECT_EQ(0, provider.availabilityLevels());
    EXPECT_EQ(TileAvailabilityState::NotAvailable,
              provider.availabilityState(TileKey{"Geographic-TMS", 1, 0, 0}));
}

TEST(QuantizedMeshTerrainProviderTest, MetadataAvailabilityUpdateStartsBelowSubtreeTileLikeCesiumNative) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "maxzoom": 10,
      "metadataAvailability": 2
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));

    const TileKey subtreeKey{"Geographic-TMS", 2, 0, 0};
    const TileKey childKey{"Geographic-TMS", 3, 0, 0};
    const TileKey siblingKey{"Geographic-TMS", 3, 1, 0};
    EXPECT_EQ(TileAvailabilityState::Unknown,
              provider.availabilityState(childKey));

    DecodedHeightmap heightmap;
    DecodedHeightmap::QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 0;
    update.subtreeKey = subtreeKey;
    update.metadataAvailability = {{0, 0, 0, 0, 0}};
    heightmap.quantizedMeshAvailabilityUpdates.push_back(update);

    provider.applyAvailabilityUpdates(heightmap);

    EXPECT_EQ(TileAvailabilityState::Available,
              provider.availabilityState(childKey));
    EXPECT_EQ(TileAvailabilityState::NotAvailable,
              provider.availabilityState(siblingKey));
}

TEST(QuantizedMeshTerrainProviderTest, InvalidMetadataAvailabilityUpdateLayerDoesNotMutateState) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "maxzoom": 10,
      "metadataAvailability": 2
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));

    const TileKey subtreeKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    EXPECT_EQ(TileAvailabilityState::Unknown,
              provider.availabilityState(childKey));

    DecodedHeightmap heightmap;
    DecodedHeightmap::QuantizedMeshAvailabilityUpdate negativeUpdate;
    negativeUpdate.layerIndex = -1;
    negativeUpdate.subtreeKey = subtreeKey;
    negativeUpdate.metadataAvailability = {{0, 0, 0, 0, 0}};
    heightmap.quantizedMeshAvailabilityUpdates.push_back(negativeUpdate);

    DecodedHeightmap::QuantizedMeshAvailabilityUpdate outOfRangeUpdate;
    outOfRangeUpdate.layerIndex = 1;
    outOfRangeUpdate.subtreeKey = subtreeKey;
    outOfRangeUpdate.metadataAvailability = {{0, 0, 0, 0, 0}};
    heightmap.quantizedMeshAvailabilityUpdates.push_back(outOfRangeUpdate);

    provider.applyAvailabilityUpdates(heightmap);

    EXPECT_EQ(TileAvailabilityState::Unknown,
              provider.availabilityState(childKey));
    EXPECT_FALSE(provider.isSubtreeLoaded(0, 0));
}

TEST(QuantizedMeshTerrainProviderTest, NormalizesDotSlashRelativeTileTemplate) {
    QuantizedMeshTerrainProvider provider("https://example.com/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["./{z}/{x}/{y}.terrain"],
      "minzoom": 0,
      "maxzoom": 12
    })json";

    ASSERT_TRUE(provider.configureFromLayerJson(layerJson, "http://192.168.1.8:8092/layer.json"));
    EXPECT_EQ("http://192.168.1.8:8092/{z}/{x}/{y}.terrain",
              provider.urlTemplate());
}

// ============================================================
// Terrarium 解码
// ============================================================

TEST(HeightmapTerrainProviderTest, DecodeTerrariumSeaLevel) {
    // Terrarium: height = R*256 + G + B/256 - 32768
    // Sea level (0m): R=128, G=0, B=0
    // 128*256 + 0 + 0/256 - 32768 = 32768 - 32768 = 0
    HeightmapTerrainProvider provider("https://example.com/{z}/{x}/{y}.png");
    MockImagePlatformBridge bridge({
        128, 0, 0,    // 0m
        128, 1, 0,    // 1m
        127, 255, 0,  // -1m
        128, 0, 128   // 0.5m
    });
    provider.setPlatformBridge(&bridge);

    const uint8_t encodedBytes[] = {0};
    auto decoded = provider.decodeTile(encodedBytes, sizeof(encodedBytes));

    ASSERT_NE(nullptr, decoded);
    ASSERT_EQ(2, decoded->tileSize);
    ASSERT_EQ(4u, decoded->heights.size());
    EXPECT_FLOAT_EQ(0.0f, decoded->heights[0]);
    EXPECT_FLOAT_EQ(1.0f, decoded->heights[1]);
    EXPECT_FLOAT_EQ(-1.0f, decoded->heights[2]);
    EXPECT_FLOAT_EQ(0.5f, decoded->heights[3]);
    EXPECT_FLOAT_EQ(-1.0f, decoded->minHeight);
    EXPECT_FLOAT_EQ(1.0f, decoded->maxHeight);
}

// ============================================================
// DecodedHeightmap
// ============================================================

TEST(DecodedHeightmapTest, ValidCheck) {
    DecodedHeightmap hm;
    EXPECT_FALSE(hm.valid());

    hm.tileSize = 2;
    hm.heights = {0, 1, 2, 3};
    EXPECT_TRUE(hm.valid());
}

TEST(DecodedHeightmapTest, BilinearCenter) {
    DecodedHeightmap hm;
    hm.tileSize = 2;
    hm.heights = {0.0f, 10.0f,   // row 0 (north)
                   0.0f, 10.0f};  // row 1 (south)

    // Center (0.5, 0.5) → average of all 4 corners = 5.0
    float h = hm.sampleBilinear(0.5f, 0.5f);
    EXPECT_FLOAT_EQ(5.0f, h);
}

TEST(DecodedHeightmapTest, BilinearCorner) {
    DecodedHeightmap hm;
    hm.tileSize = 256;
    hm.heights.resize(256 * 256, 100.0f);
    hm.heights[0] = 42.0f;

    // Top-left corner (0, 0) → first pixel
    float h = hm.sampleBilinear(0.0f, 0.0f);
    EXPECT_FLOAT_EQ(42.0f, h);
}

TEST(DecodedHeightmapTest, BilinearOutOfRangeClamped) {
    DecodedHeightmap hm;
    hm.tileSize = 2;
    hm.heights = {0, 10, 0, 10};

    // Out of range should clamp: (-0.5, 2.0) → (0, 1.0) = bottom-left corner = 0
    float h = hm.sampleBilinear(-0.5f, 2.0f);
    EXPECT_FLOAT_EQ(0.0f, h);

    // (2.0, -1.0) → (1, 0) = top-right corner = 10
    float h2 = hm.sampleBilinear(2.0f, -1.0f);
    EXPECT_FLOAT_EQ(10.0f, h2);
}

TEST(DecodedHeightmapTest, NoDataMatchesOpenGlobusRgbTerrain) {
    DecodedHeightmap hm;
    hm.noDataValues = {-32768.0f, -10000.0f};

    EXPECT_TRUE(hm.isNoData(50000.5f));
    EXPECT_TRUE(hm.isNoData(-32768.0f));
    EXPECT_TRUE(hm.isNoData(-10000.0f));
    EXPECT_FALSE(hm.isNoData(50000.0f));
    EXPECT_FALSE(hm.isNoData(123.0f));
}

TEST(TerrainTileTest, NoDataFallsBackToLowZoomParentSeaLevel) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey parentKey{"XYZ-WebMercator", 8, 120, 88};
    const TileKey childKey{"XYZ-WebMercator", 9, 240, 176};

    TerrainTile parent(
        parentKey,
        *scheme,
        makeFlatHeightmapForProviderTest(123.0f));
    TerrainTile child(
        childKey,
        *scheme,
        makeFlatHeightmapForProviderTest(60001.0f));

    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    const double lng = (childBounds.west() + childBounds.east()) * 0.5;
    const double lat = (childBounds.south() + childBounds.north()) * 0.5;

    EXPECT_FLOAT_EQ(0.0f, child.sampleHeight(lng, lat, &parent));
}

TEST(TerrainTileTest, NoDataFallsBackToHighZoomParentHeight) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey parentKey{"XYZ-WebMercator", 9, 240, 176};
    const TileKey childKey{"XYZ-WebMercator", 10, 480, 352};

    TerrainTile parent(
        parentKey,
        *scheme,
        makeFlatHeightmapForProviderTest(123.0f));
    TerrainTile child(
        childKey,
        *scheme,
        makeFlatHeightmapForProviderTest(60001.0f));

    const Rectangle childBounds = scheme->tileToRectangle(childKey);
    const double lng = (childBounds.west() + childBounds.east()) * 0.5;
    const double lat = (childBounds.south() + childBounds.north()) * 0.5;

    EXPECT_FLOAT_EQ(123.0f, child.sampleHeight(lng, lat, &parent));
}

TEST(XYZImageryProviderTest, RejectsOpenGlobusTileUnlessGroupedYEnabled) {
    XYZImageryProvider provider("https://example.com/{z}/{x}/{y}.png");
    TileKey polar{"OpenGlobus-Earth", 3, 4, 12};

    EXPECT_FALSE(provider.supportsTile(polar));
}

TEST(XYZImageryProviderTest, GeographicSchemeUsesCesiumNativeTwoByOneRoot) {
    XYZImageryProvider provider("https://example.com/{z}/{x}/{y}.png");
    provider.setSchemeId("Geographic-TMS");

    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 1, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 2, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 1}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 0, 0}));
}

TEST(XYZImageryProviderTest, OpenGlobusGroupedYMapsUrlLocalYAndExposesGroup) {
    XYZImageryProvider provider(
        "https://example.com/{tileGroup}/{z}/{x}/{y}?gy={groupedY}");
    provider.setOpenGlobusGroupedY(true);

    TileKey north{"OpenGlobus-Earth", 3, 4, 12};
    TileKey south{"OpenGlobus-Earth", 3, 4, 20};

    EXPECT_TRUE(provider.supportsTile(north));
    EXPECT_EQ("https://example.com/north/3/4/4?gy=12", provider.buildUrl(north));
    EXPECT_EQ("https://example.com/south/3/4/4?gy=20", provider.buildUrl(south));
}

TEST(XYZImageryProviderTest, OpenGlobusGroupedYRejectsOutOfRangeX) {
    XYZImageryProvider provider("https://example.com/{z}/{x}/{y}.png");
    provider.setOpenGlobusGroupedY(true);

    EXPECT_TRUE(provider.supportsTile(TileKey{"OpenGlobus-Earth", 3, 7, 4}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"OpenGlobus-Earth", 3, 8, 4}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"OpenGlobus-Earth", 3, -1, 4}));
}

TEST(XYZImageryProviderTest, OpenGlobusGroupedYCanRejectPolarGroupsWhenProviderLacksPolarCoverage) {
    XYZImageryProvider provider("https://example.com/{z}/{x}/{y}.png");
    provider.setOpenGlobusGroupedY(true);
    provider.setOpenGlobusPolarGroupsEnabled(false);

    TileKey mercator{"OpenGlobus-Earth", 3, 4, 4};
    TileKey north{"OpenGlobus-Earth", 3, 4, 12};
    TileKey south{"OpenGlobus-Earth", 3, 4, 20};

    EXPECT_TRUE(provider.supportsTile(mercator));
    EXPECT_FALSE(provider.supportsTile(north));
    EXPECT_FALSE(provider.supportsTile(south));
}

TEST(TileMapServiceUrlTest, AppendsTileMapResourceXmlBeforeQueryLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/tms/tilemapresource.xml",
        tileMapServiceXmlUrl("https://example.com/tms"));
    EXPECT_EQ(
        "https://example.com/tms/tilemapresource.xml?some=parameter",
        tileMapServiceXmlUrl("https://example.com/tms?some=parameter"));
}

TEST(TileMapServiceUrlTest, DoesNotAddSlashAfterExistingXmlWithQueryLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/tms/tilemapresource.xml?some=parameter",
        tileMapServiceXmlUrl(
            "https://example.com/tms/tilemapresource.xml?some=parameter"));
}

TEST(TileMapServiceUrlTest, ResolvesBesideNonTileMapXmlLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/tms/tilemapresource.xml?some=parameter#frag",
        tileMapServiceXmlUrl(
            "https://example.com/tms/other.xml?some=parameter#frag"));
}

TEST(TileMapServiceUrlTest, BuildsTileUrlFromTilesetHrefLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/tms/3/12/5.png",
        tileMapServiceTileUrl(
            "https://example.com/tms/",
            "3",
            12,
            5,
            ".png"));
    EXPECT_EQ(
        "https://example.com/tms/levels/3/12/5.jpg",
        tileMapServiceTileUrl(
            "https://example.com/tms/tilemapresource.xml?token=ignored",
            "levels/3",
            12,
            5,
            ".jpg"));
}

TEST(TileMapServiceUrlTest, BuildsTileUrlFromAbsoluteTilesetHrefLikeCesiumNative) {
    EXPECT_EQ(
        "https://cdn.example.com/tiles/3/12/5.png",
        tileMapServiceTileUrl(
            "https://example.com/tms/tilemapresource.xml",
            "https://cdn.example.com/tiles/3",
            12,
            5,
            ".png"));
}

TEST(TileMapServiceUrlTest, BuildsTileUrlForKeyWithMinimumLevelOffsetLikeCesiumNative) {
    TileMapServiceMetadata metadata;
    metadata.fileExtension = "jpg";
    metadata.minimumLevel = 4;
    metadata.maximumLevel = 5;
    metadata.tileSets = {
        TileMapServiceTileSet{"levels/4", 4},
        TileMapServiceTileSet{"levels/5", 5}};

    const std::optional<std::string> url = tileMapServiceTileUrlForKey(
        "https://example.com/tms/tilemapresource.xml?token=ignored",
        metadata,
        TileKey{"TMS-WebMercator", 5, 12, 6});

    ASSERT_TRUE(url.has_value());
    EXPECT_EQ("https://example.com/tms/levels/5/12/6.jpg", *url);
}

TEST(TileMapServiceUrlTest, ReturnsNoTileUrlWhenLevelHasNoTilesetLikeCesiumNative) {
    TileMapServiceMetadata metadata;
    metadata.fileExtension = "png";
    metadata.minimumLevel = 2;
    metadata.maximumLevel = 4;
    metadata.tileSets = {TileMapServiceTileSet{"levels/2", 2}};

    EXPECT_FALSE(tileMapServiceTileUrlForKey(
        "https://example.com/tms/tilemapresource.xml",
        metadata,
        TileKey{"TMS-WebMercator", 1, 0, 0}));
    EXPECT_FALSE(tileMapServiceTileUrlForKey(
        "https://example.com/tms/tilemapresource.xml",
        metadata,
        TileKey{"TMS-WebMercator", 4, 0, 0}));
}

TEST(TileMapServiceUrlTest, ParsesTileFormatAndTileSetsLikeCesiumNative) {
    const std::string xml = R"xml(
      <TileMap>
        <TileFormat width="128" height="64" extension="jpg" />
        <TileSets profile="global-mercator">
          <TileSet href="0" order="0" />
          <TileSet href="levels/2" order="2" />
        </TileSets>
      </TileMap>
    )xml";

    const TileMapServiceMetadata metadata =
        parseTileMapServiceMetadata(xml);

    EXPECT_EQ("jpg", metadata.fileExtension);
    EXPECT_EQ(128u, metadata.tileWidth);
    EXPECT_EQ(64u, metadata.tileHeight);
    EXPECT_EQ(0u, metadata.minimumLevel);
    EXPECT_EQ(2u, metadata.maximumLevel);
    ASSERT_EQ(2u, metadata.tileSets.size());
    EXPECT_EQ("0", metadata.tileSets[0].url);
    EXPECT_EQ(0u, metadata.tileSets[0].level);
    EXPECT_EQ("levels/2", metadata.tileSets[1].url);
    EXPECT_EQ(2u, metadata.tileSets[1].level);
}

TEST(TileMapServiceUrlTest, ParsesTileSetsDefaultsLikeCesiumNative) {
    const std::string xml = R"xml(
      <TileMap>
        <TileSets>
          <TileSet />
        </TileSets>
      </TileMap>
    )xml";

    const TileMapServiceMetadata metadata =
        parseTileMapServiceMetadata(xml);

    EXPECT_EQ("png", metadata.fileExtension);
    EXPECT_EQ(256u, metadata.tileWidth);
    EXPECT_EQ(256u, metadata.tileHeight);
    EXPECT_EQ(0u, metadata.minimumLevel);
    EXPECT_EQ(0u, metadata.maximumLevel);
    ASSERT_EQ(1u, metadata.tileSets.size());
    EXPECT_EQ("0", metadata.tileSets[0].url);
    EXPECT_EQ(0u, metadata.tileSets[0].level);
}

TEST(TileMapServiceUrlTest, DefaultsLevelsWhenNoTileSetsLikeCesiumNative) {
    const TileMapServiceMetadata metadata =
        parseTileMapServiceMetadata("<TileMap></TileMap>");

    EXPECT_EQ(0u, metadata.minimumLevel);
    EXPECT_EQ(25u, metadata.maximumLevel);
    EXPECT_TRUE(metadata.tileSets.empty());
}

TEST(TileMapServiceUrlTest, ParsesProfileSchemeLikeCesiumNative) {
    TileMapServiceMetadata metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <TileSets profile="global-mercator" />
      </TileMap>
    )xml");

    EXPECT_EQ("TMS-WebMercator", metadata.schemeId);
    EXPECT_FALSE(metadata.boundingBoxCoordinatesInDegrees);

    metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <TileSets profile="geodetic" />
      </TileMap>
    )xml");

    EXPECT_EQ("Geographic-TMS", metadata.schemeId);
    EXPECT_TRUE(metadata.boundingBoxCoordinatesInDegrees);

    metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <TileSets profile="global-geodetic" />
      </TileMap>
    )xml");

    EXPECT_EQ("Geographic-TMS", metadata.schemeId);
    EXPECT_TRUE(metadata.boundingBoxCoordinatesInDegrees);
}

TEST(TileMapServiceUrlTest, FallsBackToSrsForUnknownProfileLikeCesiumNative) {
    TileMapServiceMetadata metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <SRS>EPSG:4326</SRS>
        <TileSets profile="custom" />
      </TileMap>
    )xml");

    EXPECT_EQ("Geographic-TMS", metadata.schemeId);
    EXPECT_TRUE(metadata.boundingBoxCoordinatesInDegrees);

    metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <SRS>EPSG:900913</SRS>
        <TileSets profile="custom" />
      </TileMap>
    )xml");

    EXPECT_EQ("TMS-WebMercator", metadata.schemeId);
    EXPECT_TRUE(metadata.boundingBoxCoordinatesInDegrees);
}

TEST(TileMapServiceUrlTest, ParsesDegreesBoundingBoxAsProjectedCoverageLikeCesiumNative) {
    const TileMapServiceMetadata metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <BoundingBox minx="-10" miny="-20" maxx="30" maxy="40" />
        <TileSets profile="mercator" />
      </TileMap>
    )xml");

    ASSERT_TRUE(metadata.projectedCoverageRectangle.has_value());
    const Rectangle expected = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        Rectangle::fromDegrees(-10.0, -20.0, 30.0, 40.0));

    EXPECT_TRUE(metadata.projectedCoverageRectangle->equalsEpsilon(
        expected,
        1e-6));
}

TEST(TileMapServiceUrlTest, ParsesGlobalMercatorBoundingBoxAsProjectedLikeCesiumNative) {
    const TileMapServiceMetadata metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <BoundingBox minx="-1000" miny="-2000" maxx="3000" maxy="4000" />
        <TileSets profile="global-mercator" />
      </TileMap>
    )xml");

    ASSERT_TRUE(metadata.projectedCoverageRectangle.has_value());
    EXPECT_TRUE(metadata.projectedCoverageRectangle->equalsEpsilon(
        Rectangle(-1000.0, -2000.0, 3000.0, 4000.0),
        0.0));
}

TEST(TileMapServiceUrlTest, ConvertsProjectedCoverageToGeographicOverlayRectangle) {
    TileMapServiceMetadata metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <BoundingBox minx="-10" miny="-20" maxx="30" maxy="40" />
        <TileSets profile="mercator" />
      </TileMap>
    )xml");

    const std::optional<Rectangle> coverage =
        tileMapServiceGeographicCoverageRectangle(metadata);

    ASSERT_TRUE(coverage.has_value());
    EXPECT_TRUE(coverage->equalsEpsilon(
        Rectangle::fromDegrees(-10.0, -20.0, 30.0, 40.0),
        1e-12));

    metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <TileSets profile="global-mercator" />
      </TileMap>
    )xml");

    EXPECT_FALSE(tileMapServiceGeographicCoverageRectangle(metadata).has_value());
}

TEST(TileMapServiceImageryProviderTest, ConfiguresProviderFromMetadataLikeCesiumNative) {
    TileMapServiceMetadata metadata;
    metadata.fileExtension = "jpg";
    metadata.tileWidth = 128;
    metadata.tileHeight = 64;
    metadata.minimumLevel = 2;
    metadata.maximumLevel = 3;
    metadata.schemeId = "Geographic-TMS";
    metadata.tileSets = {
        TileMapServiceTileSet{"levels/2", 2},
        TileMapServiceTileSet{"levels/3", 3}};

    TileMapServiceImageryProvider provider(
        "https://example.com/tms/tilemapresource.xml?token=ignored",
        metadata,
        "test attribution");

    EXPECT_EQ("tms-imagery", provider.type());
    EXPECT_EQ("Geographic-TMS", provider.schemeId());
    EXPECT_EQ(2, provider.minZoom());
    EXPECT_EQ(3, provider.maxZoom());
    EXPECT_EQ(128, provider.tileWidth());
    EXPECT_EQ(64, provider.tileHeight());
    EXPECT_EQ("test attribution", provider.attribution());
    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 3, 4, 1}));
    EXPECT_EQ(
        "https://example.com/tms/levels/3/4/1.jpg",
        provider.buildUrl(TileKey{"Geographic-TMS", 3, 4, 1}));
}

TEST(TileMapServiceImageryProviderTest, RejectsTilesWithoutTilesetLikeCesiumNative) {
    TileMapServiceMetadata metadata;
    metadata.minimumLevel = 2;
    metadata.maximumLevel = 4;
    metadata.schemeId = "TMS-WebMercator";
    metadata.tileSets = {TileMapServiceTileSet{"levels/2", 2}};

    TileMapServiceImageryProvider provider(
        "https://example.com/tms/tilemapresource.xml",
        metadata);

    EXPECT_TRUE(provider.supportsTile(TileKey{"TMS-WebMercator", 2, 1, 1}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"TMS-WebMercator", 4, 1, 1}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 1, 1}));
    EXPECT_EQ("", provider.buildUrl(TileKey{"TMS-WebMercator", 4, 1, 1}));
}
