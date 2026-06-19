#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/core/math/Rectangle.h"

using namespace earth_engine;

// ============================================================
// XYZ Web Mercator Tile Scheme
// ============================================================

class TileSchemeTest : public ::testing::Test {
protected:
    void SetUp() override {
        scheme_ = TileScheme::createXYZWebMercator();
    }
    std::unique_ptr<TileScheme> scheme_;
};

TEST_F(TileSchemeTest, IdAndCrsProfile) {
    EXPECT_EQ("XYZ-WebMercator", scheme_->id());
    EXPECT_EQ("EPSG:3857", scheme_->crsProfile());  // crs().id() returns EPSG code
    EXPECT_EQ(256, scheme_->tileSize());
    EXPECT_EQ(0, scheme_->minZoom());
    EXPECT_EQ(22, scheme_->maxZoom());
    EXPECT_EQ("down", scheme_->yDirection());
}

TEST_F(TileSchemeTest, Z0CoversWorld) {
    // z=0 的单个 tile 应覆盖（近似）世界范围
    Rectangle r = scheme_->tileToRectangle(TileKey{"XYZ-WebMercator", 0, 0, 0});

    // 经度范围应接近 -180° ~ 180°
    EXPECT_NEAR(-180.0, r.westDegrees(), 0.1);
    EXPECT_NEAR(180.0, r.eastDegrees(), 0.1);

    // 纬度范围应覆盖 Web Mercator 常用范围 (~±85.05°)
    EXPECT_NEAR(-85.05, r.southDegrees(), 0.2);
    EXPECT_NEAR(85.05, r.northDegrees(), 0.2);
}

TEST_F(TileSchemeTest, PositionToTileZ0) {
    // 北京 → z=0 的 tile (必然只有 0/0/0)
    auto key = scheme_->positionToTile(
        116.397 * M_PI / 180.0, 39.908 * M_PI / 180.0, 0);
    EXPECT_EQ(0, key.z);
    EXPECT_EQ(0, key.x);
    EXPECT_EQ(0, key.y);
}

TEST_F(TileSchemeTest, TileCountsMatchCesiumNativeRootShiftSemantics) {
    EXPECT_EQ(1, scheme_->tileCountX(0));
    EXPECT_EQ(1, scheme_->tileCountY(0));
    EXPECT_EQ(8, scheme_->tileCountX(3));
    EXPECT_EQ(8, scheme_->tileCountY(3));
}

TEST_F(TileSchemeTest, PositionToTileKnownWebMercatorTile) {
    // XYZ Web Mercator, EPSG:3857 tile matrix. Beijing is in 13/6744/3104.
    auto key = scheme_->positionToTile(
        116.397 * M_PI / 180.0, 39.908 * M_PI / 180.0, 13);

    EXPECT_EQ(13, key.z);
    EXPECT_EQ(6744, key.x);
    EXPECT_EQ(3104, key.y);
}

TEST_F(TileSchemeTest, PositionToTileClampsCesiumNativeWorldEdges) {
    // Equivalent to cesium-native quadtree edge behavior: coordinates on the
    // positive edge clamp to the final tile, never overflow the level.
    auto northEast = scheme_->positionToTile(M_PI, M_PI / 2.0, 3);
    EXPECT_EQ(3, northEast.z);
    EXPECT_EQ(7, northEast.x);
    EXPECT_EQ(0, northEast.y);

    auto southWest = scheme_->positionToTile(-M_PI, -M_PI / 2.0, 3);
    EXPECT_EQ(3, southWest.z);
    EXPECT_EQ(0, southWest.x);
    EXPECT_EQ(7, southWest.y);
}

TEST_F(TileSchemeTest, PositionToTileRoundtrip) {
    double lngRad = 116.397 * M_PI / 180.0;
    double latRad = 39.908 * M_PI / 180.0;

    for (int z = 1; z <= 10; z += 3) {
        auto key = scheme_->positionToTile(lngRad, latRad, z);
        Rectangle r = scheme_->tileToRectangle(key);

        // 原始坐标应在 tile bounds 内
        EXPECT_TRUE(r.contains(lngRad, latRad))
            << "Failed at zoom " << z;
    }
}

TEST_F(TileSchemeTest, YAxisDirection) {
    // XYZ: y=0 在北侧（高纬度），y 增大向南
    // z=1: tile 0/0/0 应在北半球，tile 0/0/1 应在南半球
    Rectangle north = scheme_->tileToRectangle(TileKey{"XYZ-WebMercator", 1, 0, 0});
    Rectangle south = scheme_->tileToRectangle(TileKey{"XYZ-WebMercator", 1, 0, 1});

    EXPECT_GT(north.north(), 0.0);   // 北半球 center
    EXPECT_LT(south.south(), 0.0);   // 南半球 center
    EXPECT_GT(north.south(), south.south());  // 北 tile 在南 tile 上面
}

TEST_F(TileSchemeTest, TileRangeZoom1) {
    // z=1 世界范围 → 应为 2×2 tile
    Rectangle world = Rectangle::fromDegrees(-180, -85, 180, 85);
    int minX, minY, maxX, maxY;
    scheme_->tileRange(world, 1, minX, minY, maxX, maxY);

    EXPECT_EQ(0, minX);
    EXPECT_EQ(1, maxX);
    EXPECT_EQ(0, minY);
    EXPECT_EQ(1, maxY);
}

TEST_F(TileSchemeTest, LevelResolution) {
    // z=0 分辨率 = 2π rad (360°)
    EXPECT_NEAR(2.0 * M_PI, scheme_->levelResolution(0), 1e-6);

    // z=1 分辨率 = π rad (180°)
    EXPECT_NEAR(M_PI, scheme_->levelResolution(1), 1e-6);

    // z=2 分辨率 = π/2 rad (90°)
    EXPECT_NEAR(M_PI / 2.0, scheme_->levelResolution(2), 1e-6);
}

TEST(TileSchemeCountTest, WebMercatorTmsUsesOneByOneRoot) {
    auto scheme = TileScheme::createTMS();

    EXPECT_EQ(1, scheme->tileCountX(0));
    EXPECT_EQ(1, scheme->tileCountY(0));
    EXPECT_EQ(16, scheme->tileCountX(4));
    EXPECT_EQ(16, scheme->tileCountY(4));
}

TEST(TileSchemeCountTest, GeographicTmsUsesTwoByOneRoot) {
    auto scheme = TileScheme::createGeographicTMS();

    EXPECT_EQ(2, scheme->tileCountX(0));
    EXPECT_EQ(1, scheme->tileCountY(0));
    EXPECT_EQ(16, scheme->tileCountX(3));
    EXPECT_EQ(8, scheme->tileCountY(3));
}

TEST(TileSchemeCountTest, OpenGlobusGroupedSchemeReportsPhysicalYRange) {
    auto scheme = TileScheme::createOpenGlobusEarth();

    EXPECT_EQ(1, scheme->tileCountX(0));
    EXPECT_EQ(3, scheme->tileCountY(0));
    EXPECT_EQ(4, scheme->tileCountX(2));
    EXPECT_EQ(12, scheme->tileCountY(2));
}

TEST(OpenGlobusEarthTileSchemeTest, SplitsMercatorAndPolarGroups) {
    auto scheme = TileScheme::createOpenGlobusEarth();

    TileKey beijing = scheme->positionToTile(
        116.397 * M_PI / 180.0, 39.908 * M_PI / 180.0, 3);
    TileKey north = scheme->positionToTile(0.0, 89.0 * M_PI / 180.0, 3);
    TileKey south = scheme->positionToTile(0.0, -89.0 * M_PI / 180.0, 3);

    const int n = 1 << 3;
    EXPECT_EQ("OpenGlobus-Earth", scheme->id());
    EXPECT_LT(beijing.y, n);
    EXPECT_GE(north.y, n);
    EXPECT_LT(north.y, 2 * n);
    EXPECT_GE(south.y, 2 * n);
    EXPECT_LT(south.y, 3 * n);

    Rectangle northBounds = scheme->tileToRectangle(north);
    Rectangle southBounds = scheme->tileToRectangle(south);
    EXPECT_GT(northBounds.north(), 85.0 * M_PI / 180.0);
    EXPECT_LT(southBounds.south(), -85.0 * M_PI / 180.0);
}

TEST(OpenGlobusEarthTileSchemeTest, PolarRoundtripContainsPosition) {
    auto scheme = TileScheme::createOpenGlobusEarth();
    const double lng = 45.0 * M_PI / 180.0;
    const double lat = 88.0 * M_PI / 180.0;

    TileKey key = scheme->positionToTile(lng, lat, 4);
    Rectangle bounds = scheme->tileToRectangle(key);

    EXPECT_TRUE(bounds.contains(lng, lat));
}
