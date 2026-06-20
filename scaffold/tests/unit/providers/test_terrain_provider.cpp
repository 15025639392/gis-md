#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/providers/HeightmapTerrainProvider.h"
#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/providers/XYZImageryProvider.h"
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

TEST(QuantizedMeshTerrainProviderTest, AvailabilityUsesInclusiveTileCenterLikeCesiumNative) {
    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/{z}/{x}/{y}.terrain");
    provider.setZoomRange(0, 10);

    provider.addAvailabilityRects(2, {{{0, 0, 0, 0}}});

    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 1, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 1, 1, 0}));
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
