#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include "earth_engine/providers/BingMapsImageryProvider.h"
#include "earth_engine/providers/GoogleMapTilesImageryProvider.h"
#include "earth_engine/providers/HeightmapTerrainProvider.h"
#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/providers/TileMapServiceImageryProvider.h"
#include "earth_engine/providers/TileMapServiceUrl.h"
#include "earth_engine/providers/WebMapServiceImageryProvider.h"
#include "earth_engine/providers/WebMapTileServiceImageryProvider.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/platform/bridge/PlatformBridge.h"
#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TileScheme.h"

using namespace earth_engine;

namespace {

class NoopHttpRequest final : public HttpRequest {
public:
    void cancel() override {}
};

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

class QueuedGoogleMapTilesPlatformBridge : public PlatformBridge {
public:
    explicit QueuedGoogleMapTilesPlatformBridge(
        std::map<std::string, std::vector<uint8_t>> responses)
        : responses_(std::move(responses)) {}

    void onMemoryPressure() override {}
    void onEnterBackground() override {}
    void onEnterForeground() override {}

    std::unique_ptr<HttpRequest> get(
        const std::string& url,
        std::function<void(int, std::vector<uint8_t>)> callback,
        HttpRequestOptions = {}) override {
        requestedUrls.push_back(url);
        auto it = responses_.find(url);
        if (it == responses_.end()) {
            callback(404, {});
        } else {
            callback(200, it->second);
        }
        return std::make_unique<NoopHttpRequest>();
    }

    std::string cacheDirectory() const override { return {}; }
    std::string documentsDirectory() const override { return {}; }

    std::unique_ptr<DecodedImage> decodeImage(
        const uint8_t*,
        size_t) override {
        auto image = std::make_unique<DecodedImage>();
        image->width = 1;
        image->height = 1;
        image->channels = 4;
        image->pixels = {1, 2, 3, 255};
        return image;
    }

    void log(LogLevel, const std::string&, const std::string&) override {}
    DeviceInfo deviceInfo() const override { return {}; }
    std::string getToken(const std::string&) const override { return {}; }

    std::vector<std::string> requestedUrls;

private:
    std::map<std::string, std::vector<uint8_t>> responses_;
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

TEST(XYZImageryProviderTest, GeographicProjectedPlaceholdersUseCesiumNativeProjection) {
    XYZImageryProvider provider(
        "https://example.com?minx={minimumX}&miny={minimumY}&maxx={maximumX}&maxy={maximumY}");
    provider.setSchemeId("Geographic-TMS");

    auto scheme = TileScheme::createGeographicTMS();
    const TileKey key{"Geographic-TMS", 0, 1, 0};
    const Rectangle projected = projectRectangleSimple(
        GeographicProjection(Ellipsoid::WGS84()),
        scheme->tileToRectangle(key));

    EXPECT_EQ(
        "https://example.com?minx=" + std::to_string(projected.west()) +
            "&miny=" + std::to_string(projected.south()) +
            "&maxx=" + std::to_string(projected.east()) +
            "&maxy=" + std::to_string(projected.north()),
        provider.buildUrl(key));
}

TEST(XYZImageryProviderTest, ReverseZUsesProviderMaximumLevelLikeCesiumNative) {
    XYZImageryProvider provider("https://example.com/{reverseZ}/{z}.png");
    provider.setZoomRange(0, 12);

    EXPECT_EQ("https://example.com/9/3.png",
              provider.buildUrl(TileKey{"XYZ-WebMercator", 3, 0, 0}));
}

TEST(XYZImageryProviderTest, UnknownPlaceholderMatchesCesiumNativeSentinel) {
    XYZImageryProvider provider("https://example.com/{nope}/{x}.png");

    EXPECT_EQ("https://example.com/[UNKNOWN PLACEHOLDER]/0.png",
              provider.buildUrl(TileKey{"XYZ-WebMercator", 0, 0, 0}));
}

TEST(XYZImageryProviderTest, PlaceholdersAreCaseInsensitiveLikeCesiumNative) {
    XYZImageryProvider provider(
        "https://example.com/{X}/{Y}/{Z}/{ReverseY}.png");

    EXPECT_EQ("https://example.com/1/2/3/5.png",
              provider.buildUrl(TileKey{"XYZ-WebMercator", 3, 1, 2}));
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

TEST(WebMapServiceImageryProviderTest, BuildsGetMapUrlLikeCesiumNative) {
    WebMapServiceImageryOptions options;
    options.layers = "land,labels";
    WebMapServiceImageryProvider provider(
        "https://example.com/wms",
        options,
        "wms attribution");

    EXPECT_EQ("wms-imagery", provider.type());
    EXPECT_EQ("Geographic-TMS", provider.schemeId());
    EXPECT_EQ(0, provider.minZoom());
    EXPECT_EQ(14, provider.maxZoom());
    EXPECT_EQ(256, provider.tileWidth());
    EXPECT_EQ(256, provider.tileHeight());
    EXPECT_EQ("wms attribution", provider.attribution());
    EXPECT_EQ(
        "https://example.com/wms?crs=EPSG:4326&styles=&transparent=true&service=WMS&request=GetMap&version=1.3.0&bbox=-90.000000,0.000000,90.000000,180.000000&layers=land,labels&format=image/png&width=256&height=256",
        provider.buildUrl(TileKey{"Geographic-TMS", 0, 1, 0}));
}

TEST(WebMapServiceImageryProviderTest, PreservesUserDefaultParametersLikeCesiumNative) {
    WebMapServiceImageryOptions options;
    options.version = "1.1.1";
    options.layers = "imagery";
    options.format = "image/jpeg";
    options.tileWidth = 512;
    options.tileHeight = 128;

    WebMapServiceImageryProvider provider(
        "https://example.com/wms?crs=EPSG:3857&styles=default&transparent=false&service=Custom&request=Old&bbox=old#frag",
        options);

    EXPECT_EQ(
        "https://example.com/wms?crs=EPSG:3857&styles=default&transparent=false&service=Custom&request=GetMap&bbox=-90.000000,-180.000000,90.000000,0.000000&version=1.1.1&layers=imagery&format=image/jpeg&width=512&height=128#frag",
        provider.buildUrl(TileKey{"Geographic-TMS", 0, 0, 0}));
}

TEST(WebMapServiceImageryProviderTest, RejectsUnsupportedTiles) {
    WebMapServiceImageryProvider provider("https://example.com/wms");

    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 1, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 2, 0}));
    EXPECT_EQ("", provider.buildUrl(TileKey{"XYZ-WebMercator", 0, 0, 0}));
}

TEST(WebMapServiceImageryProviderTest, ValidatesCapabilitiesServiceLikeCesiumNative) {
    WebMapServiceImageryOptions options;

    WebMapServiceCapabilitiesValidation validation =
        validateWebMapServiceCapabilities("<WMS_Capabilities />", options);
    EXPECT_FALSE(validation.valid);
    EXPECT_EQ(
        "Web map service XML document does not have a Service element. ",
        validation.error);

    validation = validateWebMapServiceCapabilities(
        "<WMS_Capabilities><Service /></WMS_Capabilities>",
        options);
    EXPECT_FALSE(validation.valid);
    EXPECT_EQ(
        "Invalid web map service XML document (Service > Name is missing) ",
        validation.error);

    validation = validateWebMapServiceCapabilities(
        "<WMS_Capabilities><Service><Name>WMS</Name></Service></WMS_Capabilities>",
        options);
    EXPECT_TRUE(validation.valid);
    EXPECT_EQ("", validation.error);
}

TEST(WebMapServiceImageryProviderTest, ValidatesCapabilitiesTileSizeLikeCesiumNative) {
    WebMapServiceImageryOptions options;
    options.tileWidth = 512;
    options.tileHeight = 256;

    WebMapServiceCapabilitiesValidation validation =
        validateWebMapServiceCapabilities(R"xml(
          <WMS_Capabilities>
            <Service>
              <Name>WMS</Name>
              <MaxWidth>256</MaxWidth>
            </Service>
          </WMS_Capabilities>
        )xml",
                                          options);
    EXPECT_FALSE(validation.valid);
    EXPECT_EQ(
        "configured tile width (512) exceeds Service >> MaxWidth defined in WMS document (256).",
        validation.error);

    validation = validateWebMapServiceCapabilities(R"xml(
      <WMS_Capabilities>
        <Service>
          <Name>WMS</Name>
          <MaxHeight>128</MaxHeight>
        </Service>
      </WMS_Capabilities>
    )xml",
                                                   options);
    EXPECT_FALSE(validation.valid);
    EXPECT_EQ(
        "configured tile height (256) exceeds Service >> MaxHeight defined in WMS document (128).",
        validation.error);
}

TEST(WebMapServiceImageryProviderTest, ValidatesCapabilitiesLayerLimitLikeCesiumNative) {
    WebMapServiceImageryOptions options;
    options.layers = "imagery,,labels,roads";

    WebMapServiceCapabilitiesValidation validation =
        validateWebMapServiceCapabilities(R"xml(
          <WMS_Capabilities>
            <Service>
              <Name>WMS</Name>
              <LayerLimit>2</LayerLimit>
            </Service>
          </WMS_Capabilities>
        )xml",
                                          options);

    EXPECT_FALSE(validation.valid);
    EXPECT_EQ(
        "the number of configured layers (3) exceeds WMS LayerLimit 2",
        validation.error);
}

TEST(WebMapServiceImageryProviderTest, RejectsInvalidCapabilitiesNumbersLikeCesiumNative) {
    WebMapServiceImageryOptions options;

    WebMapServiceCapabilitiesValidation validation =
        validateWebMapServiceCapabilities(R"xml(
          <WMS_Capabilities>
            <Service>
              <Name>WMS</Name>
              <MaxWidth>not-a-number</MaxWidth>
            </Service>
          </WMS_Capabilities>
        )xml",
                                          options);

    EXPECT_FALSE(validation.valid);
    EXPECT_EQ("Invalid web map service XML document", validation.error);
}

TEST(WebMapServiceImageryProviderTest, BuildsCapabilitiesUrlLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/wms?request=GetCapabilities&version=1.3.0&service=WMS",
        webMapServiceCapabilitiesUrl("https://example.com/wms", "1.3.0"));

    EXPECT_EQ(
        "https://example.com/wms?token=abc&request=GetCapabilities&version=1.1.1&service=WMS#frag",
        webMapServiceCapabilitiesUrl(
            "https://example.com/wms?token=abc&request=Old&version=0&service=Other#frag",
            "1.1.1"));
}

TEST(WebMapTileServiceImageryProviderTest, BuildsKvpUrlLikeCesiumNative) {
    WebMapTileServiceImageryOptions options;
    options.format = "image/png";
    options.layer = "imagery";
    options.style = "default";
    options.tileMatrixSetId = "GoogleMapsCompatible";
    options.tileMatrixLabels = std::vector<std::string>{"zero", "one", "two"};
    options.subdomains = {"a", "b"};
    options.dimensions = std::map<std::string, std::string>{
        {"time", "2026-06-21"},
    };

    WebMapTileServiceImageryProvider provider(
        "https://example.com/wmts?token=abc",
        options,
        "wmts attribution");

    EXPECT_TRUE(provider.usesKeyValueParameters());
    EXPECT_EQ("wmts-imagery", provider.type());
    EXPECT_EQ("XYZ-WebMercator", provider.schemeId());
    EXPECT_EQ(0, provider.minZoom());
    EXPECT_EQ(25, provider.maxZoom());
    EXPECT_EQ(256, provider.tileWidth());
    EXPECT_EQ(256, provider.tileHeight());
    EXPECT_EQ("wmts attribution", provider.attribution());
    EXPECT_EQ(
        "https://example.com/wmts?token=abc&request=GetTile&version=1.0.0&service=WMTS&format=image%2Fpng&layer=imagery&style=default&tilematrixset=GoogleMapsCompatible&tilematrix=two&tilerow=1&tilecol=1",
        provider.buildUrl(TileKey{"XYZ-WebMercator", 2, 1, 2}));
}

TEST(WebMapTileServiceImageryProviderTest, BuildsRestTemplateUrlLikeCesiumNative) {
    WebMapTileServiceImageryOptions options;
    options.layer = "base layer";
    options.style = "default";
    options.tileMatrixSetId = "matrix";
    options.subdomains = {"a", "b", "c"};
    options.dimensions = std::map<std::string, std::string>{
        {"Time", "2026-06-21T00:00:00Z"},
    };

    WebMapTileServiceImageryProvider provider(
        "https://{s}.example.com/{Layer}/{Style}/{TileMatrixSet}/{TileMatrix}/{TileRow}/{TileCol}/{Time}/{Unknown}.png",
        options);

    EXPECT_FALSE(provider.usesKeyValueParameters());
    EXPECT_EQ(
        "https://b.example.com/base%20layer/default/matrix/3/5/2/2026-06-21T00%3A00%3A00Z/{Unknown}.png",
        provider.buildUrl(TileKey{"XYZ-WebMercator", 3, 2, 2}));
}

TEST(WebMapTileServiceImageryProviderTest, SupportsGeographicTilingLikeCesiumNative) {
    WebMapTileServiceImageryOptions options;
    options.schemeId = "Geographic-TMS";
    options.layer = "imagery";
    options.style = "default";
    options.tileMatrixSetId = "EPSG:4326";

    WebMapTileServiceImageryProvider provider(
        "https://example.com/wmts",
        options);

    EXPECT_EQ("Geographic-TMS", provider.schemeId());
    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_TRUE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 1, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 2, 0}));
    EXPECT_EQ(
        "https://example.com/wmts?request=GetTile&version=1.0.0&service=WMTS&format=image%2Fjpeg&layer=imagery&style=default&tilematrixset=EPSG%3A4326&tilematrix=0&tilerow=0&tilecol=1",
        provider.buildUrl(TileKey{"Geographic-TMS", 0, 1, 0}));
}

TEST(WebMapTileServiceImageryProviderTest, RejectsUnsupportedTiles) {
    WebMapTileServiceImageryProvider provider("https://example.com/wmts");

    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 1, 0}));
    EXPECT_EQ("", provider.buildUrl(TileKey{"Geographic-TMS", 0, 0, 0}));
}

TEST(BingMapsImageryProviderTest, BuildsQuadkeyUrlLikeCesiumNative) {
    BingMapsImageryOptions options;
    options.culture = "en-US";
    options.subdomains = {"t0", "t1", "t2"};
    options.maximumLevel = 5;

    BingMapsImageryProvider provider(
        "https://dev.virtualearth.net/",
        "https://ecn.{subdomain}.tiles.virtualearth.net/tiles/a{quadkey}.jpeg?g=1&mkt={culture}",
        options,
        "bing attribution");

    EXPECT_EQ("bing-maps-imagery", provider.type());
    EXPECT_EQ("XYZ-WebMercator", provider.schemeId());
    EXPECT_EQ(0, provider.minZoom());
    EXPECT_EQ(5, provider.maxZoom());
    EXPECT_EQ(256, provider.tileWidth());
    EXPECT_EQ(256, provider.tileHeight());
    EXPECT_EQ("bing attribution", provider.attribution());
    EXPECT_EQ("011", BingMapsImageryProvider::tileXYToQuadKey(2, 3, 0));
    EXPECT_EQ(
        "https://ecn.t2.tiles.virtualearth.net/tiles/a011.jpeg?g=1&mkt=en-US&n=z",
        provider.buildUrl(TileKey{"XYZ-WebMercator", 2, 3, 3}));
}

TEST(BingMapsImageryProviderTest, PreservesExistingNAndUnknownPlaceholderLikeCesiumNative) {
    BingMapsImageryOptions options;
    options.subdomains = {"a", "b"};

    BingMapsImageryProvider provider(
        "https://example.com/root/metadata.json",
        "tiles/{subdomain}/{unknown}/{quadkey}.png?n=old",
        options);

    EXPECT_EQ(
        "https://example.com/root/tiles/a/unknown/0.png?n=old",
        provider.buildUrl(TileKey{"XYZ-WebMercator", 0, 0, 0}));
}

TEST(BingMapsImageryProviderTest, RejectsUnsupportedTiles) {
    BingMapsImageryProvider provider(
        "https://example.com/",
        "tiles/{quadkey}.png");

    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 1, 0}));
    EXPECT_EQ("", provider.buildUrl(TileKey{"Geographic-TMS", 0, 0, 0}));
}

TEST(BingMapsImageryProviderTest, InvertsYAtCesiumNativeMaximumLevel) {
    BingMapsImageryProvider provider(
        "https://example.com/",
        "tiles/{quadkey}.png");

    EXPECT_EQ(
        "https://example.com/tiles/0" + std::string(30, '2') + ".png?n=z",
        provider.buildUrl(TileKey{"XYZ-WebMercator", 30, 0, 0}));
}

TEST(BingMapsImageryProviderTest, BuildsMetadataUrlLikeCesiumNative) {
    EXPECT_EQ(
        "https://dev.virtualearth.net/REST/v1/Imagery/Metadata/Aerial?incl=ImageryProviders&key=abc&uriScheme=https",
        bingMapsMetadataUrl(
            "https://dev.virtualearth.net/",
            "Aerial",
            "abc"));
    EXPECT_EQ(
        "https://dev.virtualearth.net/root/REST/v1/Imagery/Metadata/Road?incl=ImageryProviders&key=a%20b&uriScheme=https&culture=zh-CN",
        bingMapsMetadataUrl(
            "https://dev.virtualearth.net/root/session.json?old=1",
            "Road",
            "a b",
            "zh-CN"));
}

TEST(BingMapsImageryProviderTest, ParsesMetadataLikeCesiumNative) {
    const BingMapsMetadataParseResult result = parseBingMapsMetadata(R"json({
        "resourceSets": [{
            "resources": [{
                "imageWidth": 512,
                "imageHeight": 256,
                "zoomMax": 19,
                "imageUrl": "https://ecn.{subdomain}.tiles.virtualearth.net/tiles/a{quadkey}.jpeg",
                "imageUrlSubdomains": ["t0", 4, "t2"],
                "imageryProviders": [{
                    "attribution": "Provider A",
                    "coverageAreas": [{
                        "bbox": [-10.0, 20.0, 30.0, 40.0],
                        "zoomMin": 1,
                        "zoomMax": 9
                    }, {
                        "bbox": [1.0, 2.0, 3.0],
                        "zoomMin": 1,
                        "zoomMax": 9
                    }]
                }, {
                    "coverageAreas": []
                }]
            }]
        }]
    })json");

    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ(512, result.metadata.imageWidth);
    EXPECT_EQ(256, result.metadata.imageHeight);
    EXPECT_EQ(19, result.metadata.zoomMax);
    EXPECT_EQ(
        "https://ecn.{subdomain}.tiles.virtualearth.net/tiles/a{quadkey}.jpeg",
        result.metadata.imageUrl);
    EXPECT_EQ((std::vector<std::string>{"t0", "t2"}),
              result.metadata.imageUrlSubdomains);
    ASSERT_EQ(1u, result.metadata.credits.size());
    EXPECT_EQ("Provider A", result.metadata.credits[0].attribution);
    ASSERT_EQ(1u, result.metadata.credits[0].coverageAreas.size());
    const BingMapsCreditCoverageArea& coverage =
        result.metadata.credits[0].coverageAreas[0];
    EXPECT_DOUBLE_EQ(-10.0, coverage.southDegrees);
    EXPECT_DOUBLE_EQ(20.0, coverage.westDegrees);
    EXPECT_DOUBLE_EQ(30.0, coverage.northDegrees);
    EXPECT_DOUBLE_EQ(40.0, coverage.eastDegrees);
    EXPECT_EQ(1, coverage.zoomMin);
    EXPECT_EQ(9, coverage.zoomMax);
}

TEST(BingMapsImageryProviderTest, MetadataDefaultsMatchCesiumNative) {
    const BingMapsMetadataParseResult result = parseBingMapsMetadata(R"json({
        "resourceSets": [{
            "resources": [{
                "imageUrl": "tiles/{quadkey}.png"
            }]
        }]
    })json");

    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ(256, result.metadata.imageWidth);
    EXPECT_EQ(256, result.metadata.imageHeight);
    EXPECT_EQ(30, result.metadata.zoomMax);
    EXPECT_TRUE(result.metadata.imageUrlSubdomains.empty());
}

TEST(BingMapsImageryProviderTest, UnsafeMetadataIntegersDefaultLikeCesiumNative) {
    const BingMapsMetadataParseResult result = parseBingMapsMetadata(R"json({
        "resourceSets": [{
            "resources": [{
                "imageWidth": -1,
                "imageHeight": 9223372036854775807,
                "zoomMax": "19",
                "imageUrl": "tiles/{quadkey}.png"
            }]
        }]
    })json");

    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ(256, result.metadata.imageWidth);
    EXPECT_EQ(256, result.metadata.imageHeight);
    EXPECT_EQ(30, result.metadata.zoomMax);
}

TEST(BingMapsImageryProviderTest, SkipsCoverageWithUnsafeZoomsLikeCesiumNative) {
    const BingMapsMetadataParseResult result = parseBingMapsMetadata(R"json({
        "resourceSets": [{
            "resources": [{
                "imageUrl": "tiles/{quadkey}.png",
                "imageryProviders": [{
                    "attribution": "Provider A",
                    "coverageAreas": [{
                        "bbox": [-10.0, 20.0, 30.0, 40.0],
                        "zoomMin": -1,
                        "zoomMax": 9
                    }, {
                        "bbox": [-20.0, 30.0, 40.0, 50.0],
                        "zoomMin": 1,
                        "zoomMax": 9223372036854775807
                    }, {
                        "bbox": [-30.0, 40.0, 50.0, 60.0],
                        "zoomMin": 2,
                        "zoomMax": 10
                    }]
                }]
            }]
        }]
    })json");

    ASSERT_TRUE(result.valid) << result.error;
    ASSERT_EQ(1u, result.metadata.credits.size());
    ASSERT_EQ(1u, result.metadata.credits[0].coverageAreas.size());
    const BingMapsCreditCoverageArea& coverage =
        result.metadata.credits[0].coverageAreas[0];
    EXPECT_DOUBLE_EQ(-30.0, coverage.southDegrees);
    EXPECT_DOUBLE_EQ(40.0, coverage.westDegrees);
    EXPECT_DOUBLE_EQ(50.0, coverage.northDegrees);
    EXPECT_DOUBLE_EQ(60.0, coverage.eastDegrees);
    EXPECT_EQ(2, coverage.zoomMin);
    EXPECT_EQ(10, coverage.zoomMax);
}

TEST(BingMapsImageryProviderTest, RejectsInvalidMetadataLikeCesiumNative) {
    BingMapsMetadataParseResult result = parseBingMapsMetadata("not-json");
    EXPECT_FALSE(result.valid);
    EXPECT_NE(std::string::npos, result.error.find("Error while parsing"));

    result = parseBingMapsMetadata(R"json({
        "errorDetails": ["bad key"]
    })json");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Received an error from the Bing Maps imagery metadata service: bad key",
        result.error);

    result = parseBingMapsMetadata(R"json({
        "resourceSets": [{"resources": [{}]}]
    })json");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ("Bing Maps tile imageUrl is missing or empty.", result.error);

    result = parseBingMapsMetadata(R"json({"resourceSets": []})json");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Resources were not found in the Bing Maps imagery metadata response.",
        result.error);
}

TEST(BingMapsImageryProviderTest, CreatesSourceFromMetadataLikeCesiumNative) {
    BingMapsMetadata metadata;
    metadata.imageUrl = "tiles/{subdomain}/{quadkey}.jpeg?mkt={culture}";
    metadata.imageUrlSubdomains = {"t0", "t1"};
    metadata.imageWidth = 512;
    metadata.imageHeight = 256;
    metadata.zoomMax = 19;

    BingMapsImagerySource source = createBingMapsImagerySource(
        "https://dev.virtualearth.net/root/metadata.json",
        metadata,
        "en-US",
        "bing attribution");

    ASSERT_TRUE(source.provider);
    ASSERT_TRUE(source.scheme);
    EXPECT_EQ("XYZ-WebMercator", source.scheme->id());
    EXPECT_EQ("bing-maps-imagery", source.provider->type());
    EXPECT_EQ("XYZ-WebMercator", source.provider->schemeId());
    EXPECT_EQ(0, source.provider->minZoom());
    EXPECT_EQ(19, source.provider->maxZoom());
    EXPECT_EQ(512, source.provider->tileWidth());
    EXPECT_EQ(256, source.provider->tileHeight());
    EXPECT_EQ("bing attribution", source.provider->attribution());
    EXPECT_EQ("en-US", source.provider->options().culture);
    EXPECT_EQ((std::vector<std::string>{"t0", "t1"}),
              source.provider->options().subdomains);
    EXPECT_EQ(
        "https://dev.virtualearth.net/root/tiles/t0/0.jpeg?mkt=en-US&n=z",
        source.provider->buildUrl(TileKey{"XYZ-WebMercator", 0, 0, 0}));
}

TEST(GoogleMapTilesImageryProviderTest, BuildsExistingSessionUrlLikeCesiumNative) {
    GoogleMapTilesExistingSessionOptions options;
    options.apiBaseUrl = "https://tile.googleapis.com";
    options.session = "session-token";
    options.key = "api key";
    options.maximumLevel = 28;
    options.tileWidth = 512;
    options.tileHeight = 512;

    GoogleMapTilesImageryProvider provider(
        options,
        "google attribution");

    EXPECT_EQ("google-map-tiles-imagery", provider.type());
    EXPECT_EQ("XYZ-WebMercator", provider.schemeId());
    EXPECT_EQ(0, provider.minZoom());
    EXPECT_EQ(28, provider.maxZoom());
    EXPECT_EQ(512, provider.tileWidth());
    EXPECT_EQ(512, provider.tileHeight());
    EXPECT_EQ("google attribution", provider.attribution());
    EXPECT_EQ("https://tile.googleapis.com/",
              provider.options().apiBaseUrl);
    EXPECT_EQ(
        "https://tile.googleapis.com/v1/2dtiles/2/3/0?session=session-token&key=api%20key",
        provider.buildUrl(TileKey{"XYZ-WebMercator", 2, 3, 3}));
}

TEST(GoogleMapTilesImageryProviderTest, RejectsUnsupportedTiles) {
    GoogleMapTilesExistingSessionOptions options;
    options.session = "session";
    options.key = "key";
    options.maximumLevel = 1;
    GoogleMapTilesImageryProvider provider(options);

    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 1, 1, 1}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"Geographic-TMS", 1, 1, 1}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 0, 1, 0}));
    EXPECT_EQ("", provider.buildUrl(TileKey{"XYZ-WebMercator", 2, 0, 0}));
}

TEST(GoogleMapTilesImageryProviderTest, KnownAvailabilityRejectsUnavailableTiles) {
    GoogleMapTilesExistingSessionOptions options;
    options.session = "session";
    options.key = "key";
    options.maximumLevel = 2;
    GoogleMapTilesImageryProvider provider(options);

    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 3, 3}));
    EXPECT_FALSE(provider.hasKnownAvailability());

    provider.addAvailableTileRanges(
        {GoogleMapTilesTileRange{2, 0, 0, 1, 1},
         GoogleMapTilesTileRange{1, 0, 0, 0, 0},
         GoogleMapTilesTileRange{0, 0, 0, 0, 0}});

    EXPECT_TRUE(provider.hasKnownAvailability());
    EXPECT_TRUE(provider.isTileKnownAvailable(
        TileKey{"XYZ-WebMercator", 2, 1, 1}));
    EXPECT_FALSE(provider.isTileKnownAvailable(
        TileKey{"XYZ-WebMercator", 2, 3, 3}));
    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 1, 1}));
    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 3, 3}));

    provider.addCompleteAvailabilityRanges(
        {GoogleMapTilesTileRange{2, 0, 0, 3, 3}});
    EXPECT_TRUE(provider.isTileInCompleteAvailabilityRange(
        TileKey{"XYZ-WebMercator", 2, 3, 3}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 3, 3}));
    EXPECT_EQ("", provider.buildUrl(TileKey{"XYZ-WebMercator", 2, 3, 3}));
}

TEST(GoogleMapTilesImageryProviderTest, BuildsCreateSessionRequestLikeCesiumNative) {
    GoogleMapTilesNewSessionOptions options;
    options.apiBaseUrl = "https://tile.googleapis.com";
    options.key = "api key";
    options.mapType = "terrain";
    options.language = "zh-CN";
    options.region = "CN";
    options.imageFormat = "png";
    options.scale = "scaleFactor2x";
    options.highDpi = true;
    options.layerTypes =
        std::vector<std::string>{"layerRoadmap", "layerTraffic"};
    options.styles = nlohmann::json::array(
        {nlohmann::json{{"featureType", "road"},
                        {"elementType", "geometry"},
                        {"stylers",
                         nlohmann::json::array(
                             {nlohmann::json{{"color", "#123456"}}})}}});
    options.overlay = false;

    EXPECT_EQ(
        "https://tile.googleapis.com/v1/createSession?key=api%20key",
        googleMapTilesCreateSessionUrl(options));

    const std::string payload = googleMapTilesCreateSessionPayload(options);
    EXPECT_NE(std::string::npos, payload.find("\"mapType\":\"terrain\""));
    EXPECT_NE(std::string::npos, payload.find("\"language\":\"zh-CN\""));
    EXPECT_NE(std::string::npos, payload.find("\"region\":\"CN\""));
    EXPECT_NE(std::string::npos, payload.find("\"imageFormat\":\"png\""));
    EXPECT_NE(std::string::npos, payload.find("\"scale\":\"scaleFactor2x\""));
    EXPECT_NE(std::string::npos, payload.find("\"highDpi\":true"));
    EXPECT_NE(
        std::string::npos,
        payload.find("\"layerTypes\":[\"layerRoadmap\",\"layerTraffic\"]"));
    const nlohmann::json payloadJson = nlohmann::json::parse(payload);
    ASSERT_TRUE(payloadJson.contains("styles"));
    ASSERT_TRUE(payloadJson["styles"].is_array());
    ASSERT_EQ(1u, payloadJson["styles"].size());
    EXPECT_EQ("road", payloadJson["styles"][0]["featureType"]);
    EXPECT_EQ("geometry", payloadJson["styles"][0]["elementType"]);
    EXPECT_EQ("#123456",
              payloadJson["styles"][0]["stylers"][0]["color"]);
    EXPECT_NE(std::string::npos, payload.find("\"overlay\":false"));
}

TEST(GoogleMapTilesImageryProviderTest, CreateSessionOmitsAbsentOptionalsLikeCesiumNative) {
    GoogleMapTilesNewSessionOptions options;

    EXPECT_EQ(
        "https://tile.googleapis.com/v1/createSession",
        googleMapTilesCreateSessionUrl(options));

    const std::string payload = googleMapTilesCreateSessionPayload(options);
    EXPECT_NE(std::string::npos, payload.find("\"mapType\":\"satellite\""));
    EXPECT_NE(std::string::npos, payload.find("\"language\":\"en-US\""));
    EXPECT_NE(std::string::npos, payload.find("\"region\":\"US\""));
    EXPECT_EQ(std::string::npos, payload.find("imageFormat"));
    EXPECT_EQ(std::string::npos, payload.find("scale"));
    EXPECT_EQ(std::string::npos, payload.find("highDpi"));
    EXPECT_EQ(std::string::npos, payload.find("layerTypes"));
    EXPECT_EQ(std::string::npos, payload.find("styles"));
    EXPECT_EQ(std::string::npos, payload.find("overlay"));
}

TEST(GoogleMapTilesImageryProviderTest, ParsesCreateSessionResponseLikeCesiumNative) {
    GoogleMapTilesNewSessionOptions request;
    request.apiBaseUrl = "https://tile.googleapis.com";
    request.key = "api-key";

    const GoogleMapTilesSessionParseResult result =
        parseGoogleMapTilesCreateSessionResponse(
            R"json({
                "session": "session-token",
                "tileWidth": 512,
                "tileHeight": 256
            })json",
            request);

    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ("api-key", result.session.key);
    EXPECT_EQ("session-token", result.session.session);
    EXPECT_EQ("https://tile.googleapis.com/", result.session.apiBaseUrl);
    EXPECT_EQ(28, result.session.maximumLevel);
    EXPECT_EQ(512, result.session.tileWidth);
    EXPECT_EQ(256, result.session.tileHeight);
    EXPECT_TRUE(result.session.showLogo);
}

TEST(GoogleMapTilesImageryProviderTest, CreateSessionTileSizeFallsBackWhenNarrowingFailsLikeCesiumNative) {
    GoogleMapTilesNewSessionOptions request;

    const GoogleMapTilesSessionParseResult result =
        parseGoogleMapTilesCreateSessionResponse(
            R"json({
                "session": "session-token",
                "tileWidth": 100000000000000000000,
                "tileHeight": 512.5
            })json",
            request);

    ASSERT_TRUE(result.valid) << result.error;
    EXPECT_EQ(256, result.session.tileWidth);
    EXPECT_EQ(256, result.session.tileHeight);
}

TEST(GoogleMapTilesImageryProviderTest, RejectsInvalidCreateSessionResponseLikeCesiumNative) {
    GoogleMapTilesNewSessionOptions request;

    GoogleMapTilesSessionParseResult result =
        parseGoogleMapTilesCreateSessionResponse("not-json", request);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Failed to parse response from Google Map Tiles API createSession service:",
        result.error);

    result = parseGoogleMapTilesCreateSessionResponse("[1,2]", request);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Response from Google Map Tiles API createSession service was not a JSON object.",
        result.error);

    result = parseGoogleMapTilesCreateSessionResponse(
        R"json({"tileWidth": 256, "tileHeight": 256})json",
        request);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Response from Google Map Tiles API createSession service did not contain a valid 'session' property.",
        result.error);

    result = parseGoogleMapTilesCreateSessionResponse(
        R"json({"session": "abc", "tileHeight": 256})json",
        request);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Response from Google Map Tiles API createSession service did not contain a valid 'tileWidth' property.",
        result.error);

    result = parseGoogleMapTilesCreateSessionResponse(
        R"json({"session": "abc", "tileWidth": 256})json",
        request);
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Response from Google Map Tiles API createSession service did not contain a valid 'tileHeight' property.",
        result.error);
}

TEST(GoogleMapTilesImageryProviderTest, CreatesSourceFromExistingSessionLikeCesiumNative) {
    GoogleMapTilesExistingSessionOptions options;
    options.apiBaseUrl = "https://tile.googleapis.com";
    options.session = "session-token";
    options.key = "api-key";
    options.maximumLevel = 28;
    options.tileWidth = 512;
    options.tileHeight = 256;

    GoogleMapTilesImagerySource source = createGoogleMapTilesImagerySource(
        options,
        "google attribution");

    ASSERT_TRUE(source.provider);
    ASSERT_TRUE(source.scheme);
    EXPECT_EQ("XYZ-WebMercator", source.scheme->id());
    EXPECT_EQ("google-map-tiles-imagery", source.provider->type());
    EXPECT_EQ("XYZ-WebMercator", source.provider->schemeId());
    EXPECT_EQ(0, source.provider->minZoom());
    EXPECT_EQ(28, source.provider->maxZoom());
    EXPECT_EQ(512, source.provider->tileWidth());
    EXPECT_EQ(256, source.provider->tileHeight());
    EXPECT_EQ("google attribution", source.provider->attribution());
    EXPECT_EQ(
        "https://tile.googleapis.com/v1/2dtiles/0/0/0?session=session-token&key=api-key",
        source.provider->buildUrl(TileKey{"XYZ-WebMercator", 0, 0, 0}));
}

TEST(GoogleMapTilesImageryProviderTest, BuildsViewportUrlLikeCesiumNative) {
    GoogleMapTilesExistingSessionOptions options;
    options.apiBaseUrl = "https://tile.googleapis.com";
    options.key = "api key";
    options.session = "session-token";

    EXPECT_EQ(
        "https://tile.googleapis.com/tile/v1/viewport?session=session-token&key=api%20key&zoom=3&west=-180&south=-85.5&east=179.25&north=85.5",
        googleMapTilesViewportUrl(
            options,
            3,
            -180.0,
            -85.5,
            179.25,
            85.5));
}

TEST(GoogleMapTilesImageryProviderTest, ParsesViewportResponseLikeCesiumNative) {
    const GoogleMapTilesViewportParseResult result =
        parseGoogleMapTilesViewportResponse(R"json({
            "maxZoomRects": [
                {
                    "maxZoom": 12,
                    "west": -180.0,
                    "south": -85.0,
                    "east": 0.0,
                    "north": 85.0
                },
                {
                    "maxZoom": -1,
                    "west": 0.0,
                    "south": 0.0,
                    "east": 1.0,
                    "north": 1.0
                },
                {
                    "maxZoom": 100000000000000000000,
                    "west": 0.0,
                    "south": 0.0,
                    "east": 1.0,
                    "north": 1.0
                },
                {
                    "maxZoom": 3.5,
                    "west": 0.0,
                    "south": 0.0,
                    "east": 1.0,
                    "north": 1.0
                },
                {
                    "maxZoom": 3,
                    "west": 0.0,
                    "south": 0.0,
                    "east": 1.0
                },
                "not-a-rect"
            ]
        })json");

    ASSERT_TRUE(result.valid);
    EXPECT_TRUE(result.complete);
    ASSERT_EQ(1u, result.maxZoomRects.size());
    EXPECT_EQ(12, result.maxZoomRects[0].maxZoom);
    EXPECT_DOUBLE_EQ(-180.0, result.maxZoomRects[0].west);
    EXPECT_DOUBLE_EQ(-85.0, result.maxZoomRects[0].south);
    EXPECT_DOUBLE_EQ(0.0, result.maxZoomRects[0].east);
    EXPECT_DOUBLE_EQ(85.0, result.maxZoomRects[0].north);
}

TEST(GoogleMapTilesImageryProviderTest, ViewportResponseWithOneHundredRectsIsIncompleteLikeCesiumNative) {
    nlohmann::json response;
    response["maxZoomRects"] = nlohmann::json::array();
    for (int i = 0; i < 100; ++i) {
        response["maxZoomRects"].push_back(nlohmann::json{
            {"maxZoom", 1},
            {"west", -180.0},
            {"south", -85.0},
            {"east", 180.0},
            {"north", 85.0}});
    }

    const GoogleMapTilesViewportParseResult result =
        parseGoogleMapTilesViewportResponse(response.dump());

    ASSERT_TRUE(result.valid);
    EXPECT_FALSE(result.complete);
    EXPECT_EQ(100u, result.maxZoomRects.size());
}

TEST(GoogleMapTilesImageryProviderTest, RejectsInvalidViewportResponseLikeCesiumNative) {
    GoogleMapTilesViewportParseResult result =
        parseGoogleMapTilesViewportResponse("not-json");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Error when parsing Google Map Tiles API viewport service JSON.",
        result.error);

    result = parseGoogleMapTilesViewportResponse("[1,2]");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Google Map Tiles API viewport service JSON was not an object.",
        result.error);

    result = parseGoogleMapTilesViewportResponse("{}");
    EXPECT_FALSE(result.valid);
    EXPECT_EQ(
        "Google Map Tiles API viewport service JSON is missing the `maxZoomRects` property.",
        result.error);
}

TEST(GoogleMapTilesImageryProviderTest, ParsesViewportCopyrightLikeCesiumNative) {
    EXPECT_EQ(
        "Imagery ©2026 Provider A, Provider B",
        parseGoogleMapTilesViewportCopyright(R"json({
            "copyright": "Imagery ©2026 Provider A, Provider B"
        })json"));
    EXPECT_EQ("", parseGoogleMapTilesViewportCopyright("not-json"));
    EXPECT_EQ("", parseGoogleMapTilesViewportCopyright("{}"));
    EXPECT_EQ("",
              parseGoogleMapTilesViewportCopyright(
                  R"json({"copyright": 123})json"));
}

TEST(GoogleMapTilesImageryProviderTest, CombinesViewportCreditsLikeCesiumNative) {
    EXPECT_EQ(
        "Imagery ©2026 Provider A, Provider B, Provider C, Inc.",
        combineGoogleMapTilesCredits({
            "Imagery ©2026 Provider A, Provider B",
            "Imagery ©2026 Provider B, Provider C, Inc.",
            "",
            "Provider A"}));
}

TEST(GoogleMapTilesImageryProviderTest, LoadCreditsFetchesGlobalViewportsLikeCesiumNative) {
    GoogleMapTilesExistingSessionOptions options;
    options.apiBaseUrl = "https://tile.googleapis.com";
    options.session = "session";
    options.key = "key";
    options.maximumLevel = 2;
    GoogleMapTilesImageryProvider provider(options, "initial attribution");

    const std::string zoom0Url = googleMapTilesViewportUrl(
        options,
        0,
        -180.0,
        -90.0,
        180.0,
        90.0);
    const std::string zoom1Url = googleMapTilesViewportUrl(
        options,
        1,
        -180.0,
        -90.0,
        180.0,
        90.0);
    const std::string zoom2Url = googleMapTilesViewportUrl(
        options,
        2,
        -180.0,
        -90.0,
        180.0,
        90.0);
    const std::string zoom0Json =
        R"json({"copyright": "Imagery ©2026 Provider A"})json";
    const std::string zoom1Json =
        R"json({"copyright": "Imagery ©2026 Provider A, Provider B"})json";
    const std::string zoom2Json =
        R"json({"copyright": "Imagery ©2026 Provider C, Inc."})json";
    QueuedGoogleMapTilesPlatformBridge bridge(
        {{zoom0Url, std::vector<uint8_t>(zoom0Json.begin(), zoom0Json.end())},
         {zoom1Url, std::vector<uint8_t>(zoom1Json.begin(), zoom1Json.end())},
         {zoom2Url, std::vector<uint8_t>(zoom2Json.begin(), zoom2Json.end())}});
    provider.setPlatformBridge(&bridge);

    provider.loadCredits();

    ASSERT_EQ(3u, bridge.requestedUrls.size());
    EXPECT_EQ(zoom0Url, bridge.requestedUrls[0]);
    EXPECT_EQ(zoom1Url, bridge.requestedUrls[1]);
    EXPECT_EQ(zoom2Url, bridge.requestedUrls[2]);
    EXPECT_EQ(
        "Imagery ©2026 Provider A, Provider B, Provider C, Inc.",
        provider.attribution());
}

TEST(GoogleMapTilesImageryProviderTest, LoadCreditsPreservesAttributionWhenNoCreditsLoad) {
    GoogleMapTilesExistingSessionOptions options;
    options.apiBaseUrl = "https://tile.googleapis.com";
    options.session = "session";
    options.key = "key";
    options.maximumLevel = 1;
    GoogleMapTilesImageryProvider provider(options, "initial attribution");

    QueuedGoogleMapTilesPlatformBridge bridge({});
    provider.setPlatformBridge(&bridge);

    provider.loadCredits();

    ASSERT_EQ(2u, bridge.requestedUrls.size());
    EXPECT_EQ("initial attribution", provider.attribution());
}

TEST(GoogleMapTilesImageryProviderTest, ConvertsViewportRectsToTileRangesLikeCesiumNative) {
    GoogleMapTilesViewportParseResult viewport;
    viewport.valid = true;
    viewport.complete = true;
    viewport.maxZoomRects.push_back(
        GoogleMapTilesViewportRect{2, -180.0, -85.0, 0.0, 85.0});

    const std::vector<GoogleMapTilesTileRange> ranges =
        googleMapTilesViewportTileRanges(viewport);

    ASSERT_EQ(3u, ranges.size());
    EXPECT_EQ(2, ranges[0].level);
    EXPECT_EQ(0, ranges[0].minimumX);
    EXPECT_EQ(0, ranges[0].minimumY);
    EXPECT_EQ(2, ranges[0].maximumX);
    EXPECT_EQ(3, ranges[0].maximumY);
    EXPECT_EQ(1, ranges[1].level);
    EXPECT_EQ(0, ranges[1].minimumX);
    EXPECT_EQ(0, ranges[1].minimumY);
    EXPECT_EQ(1, ranges[1].maximumX);
    EXPECT_EQ(1, ranges[1].maximumY);
    EXPECT_EQ(0, ranges[2].level);
    EXPECT_EQ(0, ranges[2].minimumX);
    EXPECT_EQ(0, ranges[2].minimumY);
    EXPECT_EQ(0, ranges[2].maximumX);
    EXPECT_EQ(0, ranges[2].maximumY);
}

TEST(GoogleMapTilesImageryProviderTest, ViewportTileRangesClampWebMercatorLatitudeLikeCesiumNative) {
    GoogleMapTilesViewportParseResult viewport;
    viewport.valid = true;
    viewport.maxZoomRects.push_back(
        GoogleMapTilesViewportRect{1, -180.0, -90.0, 180.0, 90.0});

    const std::vector<GoogleMapTilesTileRange> ranges =
        googleMapTilesViewportTileRanges(viewport);

    ASSERT_EQ(2u, ranges.size());
    EXPECT_EQ(1, ranges[0].level);
    EXPECT_EQ(0, ranges[0].minimumX);
    EXPECT_EQ(0, ranges[0].minimumY);
    EXPECT_EQ(1, ranges[0].maximumX);
    EXPECT_EQ(1, ranges[0].maximumY);
}

TEST(GoogleMapTilesImageryProviderTest, AppliesCompleteViewportAvailabilityLikeCesiumNative) {
    GoogleMapTilesExistingSessionOptions options;
    options.session = "session";
    options.key = "key";
    options.maximumLevel = 2;
    GoogleMapTilesImageryProvider provider(options);

    GoogleMapTilesViewportParseResult viewport;
    viewport.valid = true;
    viewport.complete = true;
    viewport.maxZoomRects.push_back(
        GoogleMapTilesViewportRect{2, -180.0, 66.0, -135.0, 85.0});

    provider.applyViewportAvailability(
        viewport,
        TileKey{"XYZ-WebMercator", 1, 0, 0});

    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 0, 0}));
    EXPECT_FALSE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 1, 1}));
    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 3, 3}));
}

TEST(GoogleMapTilesImageryProviderTest, IncompleteViewportDoesNotRejectUnknownTiles) {
    GoogleMapTilesExistingSessionOptions options;
    options.session = "session";
    options.key = "key";
    options.maximumLevel = 2;
    GoogleMapTilesImageryProvider provider(options);

    GoogleMapTilesViewportParseResult viewport;
    viewport.valid = true;
    viewport.complete = false;
    viewport.maxZoomRects.push_back(
        GoogleMapTilesViewportRect{2, -180.0, 66.0, -135.0, 85.0});

    provider.applyViewportAvailability(
        viewport,
        TileKey{"XYZ-WebMercator", 1, 0, 0});

    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 0, 0}));
    EXPECT_TRUE(provider.supportsTile(TileKey{"XYZ-WebMercator", 2, 1, 1}));
}

TEST(GoogleMapTilesImageryProviderTest, RequestTileLoadsViewportBeforeUnknownTileLikeCesiumNative) {
    GoogleMapTilesExistingSessionOptions options;
    options.apiBaseUrl = "https://tile.googleapis.com";
    options.session = "session";
    options.key = "key";
    options.maximumLevel = 2;
    GoogleMapTilesImageryProvider provider(options);

    const TileKey key{"XYZ-WebMercator", 2, 0, 0};
    const std::unique_ptr<TileScheme> scheme =
        TileScheme::createXYZWebMercator();
    const Rectangle rect = scheme->tileToRectangle(key);
    const std::string viewportUrl = googleMapTilesViewportUrl(
        options,
        key.z,
        rect.westDegrees(),
        rect.southDegrees(),
        rect.eastDegrees(),
        rect.northDegrees());
    const std::string tileUrl = provider.buildUrl(key);
    const std::string viewportJson = R"json({
        "maxZoomRects": [{
            "maxZoom": 2,
            "west": -180.0,
            "south": 66.0,
            "east": -135.0,
            "north": 85.0
        }]
    })json";
    QueuedGoogleMapTilesPlatformBridge bridge(
        {{viewportUrl,
          std::vector<uint8_t>(viewportJson.begin(), viewportJson.end())},
         {tileUrl, std::vector<uint8_t>{1, 2, 3, 4}}});
    provider.setPlatformBridge(&bridge);

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool loaded = false;
    provider.requestTile(
        key,
        CancellationToken{},
        [&](const TileKey&, std::unique_ptr<DecodedImage> image) {
            std::lock_guard<std::mutex> lock(mutex);
            loaded = image != nullptr;
            done = true;
            cv.notify_one();
        });

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&]() {
            return done;
        }));
    }

    ASSERT_EQ(2u, bridge.requestedUrls.size());
    EXPECT_EQ(viewportUrl, bridge.requestedUrls[0]);
    EXPECT_EQ(tileUrl, bridge.requestedUrls[1]);
    EXPECT_TRUE(loaded);
}

TEST(GoogleMapTilesImageryProviderTest, RequestTileSkipsTileWhenCompleteViewportExcludesIt) {
    GoogleMapTilesExistingSessionOptions options;
    options.apiBaseUrl = "https://tile.googleapis.com";
    options.session = "session";
    options.key = "key";
    options.maximumLevel = 2;
    GoogleMapTilesImageryProvider provider(options);

    const TileKey key{"XYZ-WebMercator", 2, 1, 1};
    const std::unique_ptr<TileScheme> scheme =
        TileScheme::createXYZWebMercator();
    const Rectangle rect = scheme->tileToRectangle(key);
    const std::string viewportUrl = googleMapTilesViewportUrl(
        options,
        key.z,
        rect.westDegrees(),
        rect.southDegrees(),
        rect.eastDegrees(),
        rect.northDegrees());
    const std::string viewportJson = R"json({
        "maxZoomRects": [{
            "maxZoom": 2,
            "west": -180.0,
            "south": 66.0,
            "east": -135.0,
            "north": 85.0
        }]
    })json";
    QueuedGoogleMapTilesPlatformBridge bridge(
        {{viewportUrl,
          std::vector<uint8_t>(viewportJson.begin(), viewportJson.end())}});
    provider.setPlatformBridge(&bridge);

    bool loaded = true;
    provider.requestTile(
        key,
        CancellationToken{},
        [&](const TileKey&, std::unique_ptr<DecodedImage> image) {
            loaded = image != nullptr;
        });

    ASSERT_EQ(1u, bridge.requestedUrls.size());
    EXPECT_EQ(viewportUrl, bridge.requestedUrls[0]);
    EXPECT_FALSE(loaded);
}

TEST(GoogleMapTilesImageryProviderTest, RequestTileStillLoadsViewportForUnknownArea) {
    GoogleMapTilesExistingSessionOptions options;
    options.apiBaseUrl = "https://tile.googleapis.com";
    options.session = "session";
    options.key = "key";
    options.maximumLevel = 2;
    GoogleMapTilesImageryProvider provider(options);
    provider.addAvailableTileRanges(
        {GoogleMapTilesTileRange{2, 0, 0, 0, 0}});

    const TileKey key{"XYZ-WebMercator", 2, 3, 3};
    const std::unique_ptr<TileScheme> scheme =
        TileScheme::createXYZWebMercator();
    const Rectangle rect = scheme->tileToRectangle(key);
    const std::string viewportUrl = googleMapTilesViewportUrl(
        options,
        key.z,
        rect.westDegrees(),
        rect.southDegrees(),
        rect.eastDegrees(),
        rect.northDegrees());
    const std::string viewportJson = R"json({"maxZoomRects": []})json";
    QueuedGoogleMapTilesPlatformBridge bridge(
        {{viewportUrl,
          std::vector<uint8_t>(viewportJson.begin(), viewportJson.end())}});
    provider.setPlatformBridge(&bridge);

    bool loaded = true;
    provider.requestTile(
        key,
        CancellationToken{},
        [&](const TileKey&, std::unique_ptr<DecodedImage> image) {
            loaded = image != nullptr;
        });

    ASSERT_EQ(1u, bridge.requestedUrls.size());
    EXPECT_EQ(viewportUrl, bridge.requestedUrls[0]);
    EXPECT_FALSE(loaded);
}

TEST(TileMapServiceUrlTest, AppendsTileMapResourceXmlBeforeQueryLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/tms/tilemapresource.xml",
        tileMapServiceXmlUrl("https://example.com/tms"));
    EXPECT_EQ(
        "https://example.com/tms/tilemapresource.xml?some=parameter",
        tileMapServiceXmlUrl("https://example.com/tms?some=parameter"));
}

TEST(TileMapServiceUrlTest, ResolvesShortNonXmlPathBesideEndpointLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/tilemapresource.xml",
        tileMapServiceXmlUrl("https://example.com/ab"));
    EXPECT_EQ(
        "https://example.com/tilemapresource.xml?some=parameter",
        tileMapServiceXmlUrl("https://example.com/ab?some=parameter"));
}

TEST(TileMapServiceUrlTest, BuildsTileBaseFromOriginalEndpointLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/tms/",
        tileMapServiceTileBaseUrl("https://example.com/tms"));
    EXPECT_EQ(
        "https://example.com/tms/?some=parameter",
        tileMapServiceTileBaseUrl("https://example.com/tms?some=parameter"));
    EXPECT_EQ(
        "https://example.com/tms/tilemapresource.xml?some=parameter",
        tileMapServiceTileBaseUrl(
            "https://example.com/tms/tilemapresource.xml?some=parameter"));
    EXPECT_EQ(
        "https://example.com/ab?some=parameter",
        tileMapServiceTileBaseUrl("https://example.com/ab?some=parameter"));
}

TEST(TileMapServiceUrlTest, DoesNotAddSlashAfterExistingXmlWithQueryLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/tms/tilemapresource.xml?some=parameter",
        tileMapServiceXmlUrl(
            "https://example.com/tms/tilemapresource.xml?some=parameter"));
}

TEST(TileMapServiceUrlTest, DoesNotFallbackWhenTileMapResourceAppearsInQueryLikeCesiumNative) {
    EXPECT_EQ(
        "https://example.com/tms?file=tilemapresource.xml",
        tileMapServiceXmlUrl("https://example.com/tms?file=tilemapresource.xml"));
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

TEST(TileMapServiceUrlTest, BuildsTileUrlForEmptyExtensionLikeCesiumNative) {
    TileMapServiceMetadata metadata;
    metadata.fileExtension = "";
    metadata.minimumLevel = 0;
    metadata.maximumLevel = 0;
    metadata.tileSets = {TileMapServiceTileSet{"0", 0}};

    const std::optional<std::string> url = tileMapServiceTileUrlForKey(
        "https://example.com/tms/tilemapresource.xml",
        metadata,
        TileKey{"TMS-WebMercator", 0, 0, 0});

    ASSERT_TRUE(url.has_value());
    EXPECT_EQ("https://example.com/tms/0/0/0", *url);
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

TEST(TileMapServiceUrlTest, IgnoresNestedMetadataElementsLikeCesiumNative) {
    const TileMapServiceMetadata metadata =
        parseTileMapServiceMetadata(R"xml(
          <TileMap>
            <Metadata>
              <BoundingBox minx="-1000" miny="-1000" maxx="1000" maxy="1000" />
              <TileFormat width="16" height="16" extension="bad" />
              <TileSets profile="global-mercator">
                <TileSet href="nested" order="9" />
              </TileSets>
            </Metadata>
            <BoundingBox minx="-10" miny="-20" maxx="30" maxy="40" />
            <TileFormat width="128" height="64" extension="jpg" />
            <TileSets profile="global-geodetic">
              <TileSet href="direct" order="2" />
            </TileSets>
          </TileMap>
        )xml");

    EXPECT_EQ("Geographic-TMS", metadata.schemeId);
    EXPECT_EQ("jpg", metadata.fileExtension);
    EXPECT_EQ(128u, metadata.tileWidth);
    EXPECT_EQ(64u, metadata.tileHeight);
    EXPECT_EQ(2u, metadata.minimumLevel);
    EXPECT_EQ(2u, metadata.maximumLevel);
    ASSERT_EQ(1u, metadata.tileSets.size());
    EXPECT_EQ("direct", metadata.tileSets[0].url);
    EXPECT_EQ(2u, metadata.tileSets[0].level);
    ASSERT_TRUE(metadata.projectedCoverageRectangle.has_value());
    EXPECT_TRUE(metadata.projectedCoverageRectangle->equalsEpsilon(
        projectRectangleSimple(
            GeographicProjection(Ellipsoid::WGS84()),
            Rectangle::fromDegrees(-10.0, -20.0, 30.0, 40.0)),
        1e-12));
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

TEST(TileMapServiceUrlTest, LoadableXmlRejectsUnsupportedSrsLikeCesiumNative) {
    EXPECT_TRUE(tileMapServiceXmlIsLoadable(R"xml(
      <TileMap>
        <SRS>EPSG:4326</SRS>
        <TileSets profile="global-geodetic" />
      </TileMap>
    )xml"));

    EXPECT_FALSE(tileMapServiceXmlIsLoadable(R"xml(
      <TileMap>
        <SRS>EPSG:1234</SRS>
        <TileSets profile="custom" />
      </TileMap>
    )xml"));

    EXPECT_FALSE(tileMapServiceXmlIsLoadable(R"xml(
      <TileMap>
        <TileSets profile="global-geodetic" />
      </TileMap>
    )xml"));

    EXPECT_FALSE(tileMapServiceXmlIsLoadable(R"xml(
      <TileMap>
        <Metadata>
          <SRS>EPSG:4326</SRS>
          <TileSets profile="global-geodetic" />
        </Metadata>
      </TileMap>
    )xml"));

    EXPECT_FALSE(tileMapServiceXmlIsLoadable(R"xml(
      <TileMap>
        <SRS>EPSG:4326</SRS>
        <Metadata>
          <TileSets profile="global-geodetic" />
        </Metadata>
      </TileMap>
    )xml"));
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

TEST(TileMapServiceUrlTest, UnknownProfileWithoutSrsKeepsProjectedBoundingBoxLikeCesiumNative) {
    const TileMapServiceMetadata metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <BoundingBox minx="-1000" miny="-2000" maxx="3000" maxy="4000" />
        <TileSets profile="custom" />
      </TileMap>
    )xml");

    EXPECT_EQ("TMS-WebMercator", metadata.schemeId);
    EXPECT_FALSE(metadata.boundingBoxCoordinatesInDegrees);
    ASSERT_TRUE(metadata.projectedCoverageRectangle.has_value());
    EXPECT_TRUE(metadata.projectedCoverageRectangle->equalsEpsilon(
        Rectangle(-1000.0, -2000.0, 3000.0, 4000.0),
        0.0));
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

TEST(TileMapServiceUrlTest, ResolvesDefaultCoverageLikeCesiumNative) {
    TileMapServiceMetadata metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <TileSets profile="global-mercator" />
      </TileMap>
    )xml");

    EXPECT_TRUE(tileMapServiceResolvedGeographicCoverageRectangle(metadata)
                    .equalsEpsilon(
                        WebMercatorProjection::maximumGlobeRectangle(),
                        1e-12));

    metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <TileSets profile="global-geodetic" />
      </TileMap>
    )xml");

    EXPECT_TRUE(tileMapServiceResolvedGeographicCoverageRectangle(metadata)
                    .equalsEpsilon(Rectangle::MAXIMUM, 1e-12));
}

TEST(TileMapServiceUrlTest, CreatesTileSchemeFromMetadataProfile) {
    TileMapServiceMetadata metadata =
        parseTileMapServiceMetadata(R"xml(
          <TileMap>
            <TileSets profile="global-geodetic" />
          </TileMap>
        )xml");

    std::unique_ptr<TileScheme> scheme = tileMapServiceTileScheme(metadata);
    ASSERT_NE(nullptr, scheme);
    EXPECT_EQ("Geographic-TMS", scheme->id());
    EXPECT_EQ(2, scheme->tileCountX(0));
    EXPECT_EQ(1, scheme->tileCountY(0));

    metadata = parseTileMapServiceMetadata(R"xml(
      <TileMap>
        <TileSets profile="mercator" />
      </TileMap>
    )xml");

    scheme = tileMapServiceTileScheme(metadata);
    ASSERT_NE(nullptr, scheme);
    EXPECT_EQ("TMS-WebMercator", scheme->id());
    EXPECT_EQ(1, scheme->tileCountX(0));
    EXPECT_EQ(1, scheme->tileCountY(0));
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

TEST(TileMapServiceImageryProviderTest, ZoomOverrideDoesNotRewriteTilesetLevelBase) {
    TileMapServiceMetadata metadata;
    metadata.fileExtension = "png";
    metadata.minimumLevel = 2;
    metadata.maximumLevel = 4;
    metadata.schemeId = "TMS-WebMercator";
    metadata.tileSets = {
        TileMapServiceTileSet{"levels/2", 2},
        TileMapServiceTileSet{"levels/3", 3},
        TileMapServiceTileSet{"levels/4", 4}};

    TileMapServiceImageryProvider provider(
        "https://example.com/tms/tilemapresource.xml",
        metadata);
    provider.setZoomRange(3, 4);

    EXPECT_FALSE(provider.supportsTile(TileKey{"TMS-WebMercator", 2, 0, 0}));
    EXPECT_TRUE(provider.supportsTile(TileKey{"TMS-WebMercator", 3, 0, 0}));
    EXPECT_EQ(
        "https://example.com/tms/levels/3/0/0.png",
        provider.buildUrl(TileKey{"TMS-WebMercator", 3, 0, 0}));
}

TEST(TileMapServiceImageryProviderTest, CreatesSourceFromXmlForRasterOverlayInstall) {
    TileMapServiceImagerySource source = createTileMapServiceImagerySource(
        "https://example.com/tms/tilemapresource.xml",
        R"xml(
          <TileMap>
            <SRS>EPSG:4326</SRS>
            <BoundingBox minx="-10" miny="-20" maxx="30" maxy="40" />
            <TileFormat width="128" height="64" extension="jpg" />
            <TileSets profile="global-geodetic">
              <TileSet href="levels/0" order="0" />
            </TileSets>
          </TileMap>
        )xml",
        "tms attribution");

    ASSERT_NE(nullptr, source.provider);
    ASSERT_NE(nullptr, source.scheme);
    EXPECT_EQ("Geographic-TMS", source.scheme->id());
    EXPECT_EQ("Geographic-TMS", source.provider->schemeId());
    EXPECT_EQ(128, source.provider->tileWidth());
    EXPECT_EQ(64, source.provider->tileHeight());
    EXPECT_EQ("tms attribution", source.provider->attribution());
    EXPECT_EQ(
        "https://example.com/tms/levels/0/1/0.jpg",
        source.provider->buildUrl(TileKey{"Geographic-TMS", 0, 1, 0}));
    ASSERT_TRUE(source.coverageRectangle.has_value());
    EXPECT_TRUE(source.coverageRectangle->equalsEpsilon(
        Rectangle::fromDegrees(-10.0, -20.0, 30.0, 40.0),
        1e-12));
}

TEST(TileMapServiceImageryProviderTest, RejectsSourceWithoutTileSetsLikeCesiumNative) {
    TileMapServiceImagerySource source = createTileMapServiceImagerySource(
        "https://example.com/tms/tilemapresource.xml",
        R"xml(
          <TileMap>
            <SRS>EPSG:4326</SRS>
            <BoundingBox minx="-10" miny="-20" maxx="30" maxy="40" />
            <TileSets profile="global-geodetic" />
          </TileMap>
        )xml");

    EXPECT_EQ(nullptr, source.provider);
    EXPECT_EQ(nullptr, source.scheme);
    EXPECT_FALSE(source.coverageRectangle.has_value());
}

TEST(TileMapServiceImageryProviderTest, RejectsSourceWithUnsupportedSrsLikeCesiumNative) {
    TileMapServiceImagerySource source = createTileMapServiceImagerySource(
        "https://example.com/tms/tilemapresource.xml",
        R"xml(
          <TileMap>
            <SRS>EPSG:1234</SRS>
            <TileSets profile="custom">
              <TileSet href="levels/0" order="0" />
            </TileSets>
          </TileMap>
        )xml");

    EXPECT_EQ(nullptr, source.provider);
    EXPECT_EQ(nullptr, source.scheme);
    EXPECT_FALSE(source.coverageRectangle.has_value());
}
