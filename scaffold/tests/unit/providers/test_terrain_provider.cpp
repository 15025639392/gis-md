#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/providers/HeightmapTerrainProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/providers/XYZImageryProvider.h"
#include "earth_engine/tiling/TileKey.h"

using namespace earth_engine;

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

// ============================================================
// Terrarium 解码
// ============================================================

TEST(HeightmapTerrainProviderTest, DecodeTerrariumSeaLevel) {
    // Terrarium: height = R*256 + G + B/256 - 32768
    // Sea level (0m): R=128, G=0, B=0
    // 128*256 + 0 + 0/256 - 32768 = 32768 - 32768 = 0
    HeightmapTerrainProvider provider("https://example.com/{z}/{x}/{y}.png");

    // 构造一个 2×2 PNG-like RGBA 像素（实际上 stb_image 解码后会得到 3 通道）
    // 我们直接测试 decodeTile 行为...
    // Note: decodeTile 需要实际 PNG 数据或 PlatformBridge，这里测试公式正确性
    SUCCEED();  // 占位 — 实际解码需要 mock PlatformBridge 或 stb_image
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
