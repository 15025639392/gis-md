#include <gtest/gtest.h>

// The amap-vector.json parser lives in the Android demo glue. Include its
// translation unit directly so the fail-loud parsing contract is covered by
// a host test (the config file itself is device-side).
#include "../../../examples/android/MinimalGlobe/AmapVectorConfig.cpp"

using namespace earth_engine::minimal_globe_demo;

TEST(AmapVectorConfig, ParsesSourcesZoomsAndStyle) {
    const std::string json = R"({
      "sources": {
        "amap": {"apiBase": "https://alt.example/get_tile",
                  "initBase": "https://alt.example/init",
                  "iconBase": "https://alt.example/icon",
                  "sdfBase": "https://alt.example/sdf"},
        "terrain": {"urlTemplate": "https://t.example/{z}/{x}/{y}.png",
                     "minZoom": 4, "maxZoom": 10, "tileSize": 256,
                     "borderInset": 0.3}
      },
      "zooms": {
        "minZoom": 4, "regionsMaxZoom": 8, "mainMaxZoom": 12,
        "regionsActiveBelowZoom": 11.5,
        "regionsSupportedZooms": [4, 8],
        "mainSupportedZooms": [4, 8, 12],
        "poiSupportedZooms": [4, 12],
        "dataZoom": [[0, 3], [4, 4], [8, 8], [12, 12]]
      },
      "style": {
        "surface": [{"classCode": 30001, "subKey": 2, "color": "#B2CEFE"}],
        "line": [{"classCode": 20001, "subKey": 1, "color": "#FF8800",
                   "widthPx": 2.5}]
      }
    })";
    AmapVectorConfig c;
    const std::string err = parseAmapVectorConfig(json, c);
    EXPECT_TRUE(err.empty()) << err;

    EXPECT_TRUE(c.hasAmapEndpoints);
    EXPECT_EQ("https://alt.example/get_tile", c.apiBase);
    EXPECT_EQ("https://alt.example/init", c.initBase);
    EXPECT_EQ("https://alt.example/sdf", c.sdfBase);
    EXPECT_TRUE(c.hasTerrain);
    EXPECT_EQ("https://t.example/{z}/{x}/{y}.png", c.terrainUrlTemplate);
    EXPECT_EQ(4, c.terrainMinZoom);
    EXPECT_EQ(256, c.terrainTileSize);

    EXPECT_TRUE(c.hasZooms);
    EXPECT_EQ(4, c.zoomMinZoom);
    EXPECT_EQ(8, c.zoomRegionsMaxZoom);
    EXPECT_EQ(11.5, c.zoomRegionsActiveBelowZoom);
    ASSERT_EQ(2u, c.zoomRegionsSupported.size());
    EXPECT_EQ(8, c.zoomRegionsSupported[1]);
    ASSERT_EQ(4u, c.zoomDataZoomRemap.size());
    EXPECT_EQ((std::pair<int, int>{4, 4}), c.zoomDataZoomRemap[1]);

    EXPECT_TRUE(c.hasStyle);
    ASSERT_EQ(1u, c.styleOverrides.surface.size());
    EXPECT_EQ(30001, c.styleOverrides.surface[0].classCode);
    EXPECT_NEAR(0xB2 / 255.0f, c.styleOverrides.surface[0].color[0], 1e-5);
    EXPECT_NEAR(0xCE / 255.0f, c.styleOverrides.surface[0].color[1], 1e-5);
    EXPECT_NEAR(0xFE / 255.0f, c.styleOverrides.surface[0].color[2], 1e-5);
    ASSERT_EQ(1u, c.styleOverrides.line.size());
    EXPECT_NEAR(2.5f, c.styleOverrides.line[0].widthPx, 1e-5);
}

TEST(AmapVectorConfig, RejectsUnknownKeyFailLoud) {
    AmapVectorConfig c;
    const std::string err =
        parseAmapVectorConfig(R"({"sources":{"amap":{"bogus":1}}})", c);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(std::string::npos, err.find("bogus"));
}

TEST(AmapVectorConfig, EmptyFileFallsBackToDefaults) {
    AmapVectorConfig c;
    const std::string err = parseAmapVectorConfig("{}", c);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_FALSE(c.hasAmapEndpoints);
    EXPECT_FALSE(c.hasTerrain);
    EXPECT_FALSE(c.hasZooms);
    EXPECT_FALSE(c.hasStyle);
}