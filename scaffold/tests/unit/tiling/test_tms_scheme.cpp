#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/CrsProfile.h"
#include "earth_engine/core/math/Rectangle.h"

using namespace earth_engine;

// ============================================================
// TMS Web Mercator Tile Scheme
// ============================================================

class TMSTileSchemeTest : public ::testing::Test {
protected:
    void SetUp() override {
        tms_ = TileScheme::createTMS();
        xyz_ = TileScheme::createXYZWebMercator();
    }
    std::unique_ptr<TileScheme> tms_;
    std::unique_ptr<TileScheme> xyz_;
};

TEST_F(TMSTileSchemeTest, IdAndDirection) {
    EXPECT_EQ("TMS-WebMercator", tms_->id());
    EXPECT_EQ("up", tms_->yDirection());
    EXPECT_EQ("EPSG:3857", tms_->crsProfile());
    EXPECT_EQ(&CrsProfile::webMercator(), &tms_->crs());
}

TEST_F(TMSTileSchemeTest, Z0CoversWorld) {
    Rectangle r = tms_->tileToRectangle(TileKey{"TMS-WebMercator", 0, 0, 0});

    EXPECT_NEAR(-180.0, r.westDegrees(), 0.1);
    EXPECT_NEAR(180.0, r.eastDegrees(), 0.1);
    EXPECT_NEAR(-85.05, r.southDegrees(), 0.2);
    EXPECT_NEAR(85.05, r.northDegrees(), 0.2);
}

TEST_F(TMSTileSchemeTest, YAxisDirection) {
    // TMS: y=0 在南侧（南半球），y 增大向北
    // z=1: tile 0/0/0 在南半球，tile 0/0/1 在北半球
    Rectangle south = tms_->tileToRectangle(TileKey{"TMS-WebMercator", 1, 0, 0});
    Rectangle north = tms_->tileToRectangle(TileKey{"TMS-WebMercator", 1, 0, 1});

    EXPECT_LT(south.south(), 0.0);   // 南半球
    EXPECT_GT(north.north(), 0.0);   // 北半球
    EXPECT_LT(south.south(), north.north());  // 南 tile 在南边
}

TEST_F(TMSTileSchemeTest, PositionToTileRoundtrip) {
    double lngRad = 116.397 * M_PI / 180.0;
    double latRad = 39.908 * M_PI / 180.0;

    for (int z = 1; z <= 10; z += 3) {
        auto key = tms_->positionToTile(lngRad, latRad, z);
        Rectangle r = tms_->tileToRectangle(key);
        EXPECT_TRUE(r.contains(lngRad, latRad))
            << "Failed at zoom " << z;
    }
}

TEST_F(TMSTileSchemeTest, PositionToTileKnownTMS) {
    // 北京在 TMS z=13 的 tile。TMS y = (2^13-1) - XYZ y。
    // XYZ y for Beijing at z=13 ≈ 3104
    // TMS y = 8191 - 3104 = 5087
    auto key = tms_->positionToTile(
        116.397 * M_PI / 180.0, 39.908 * M_PI / 180.0, 13);

    EXPECT_EQ(13, key.z);
    EXPECT_EQ("TMS-WebMercator", key.schemeId);
    // x should match XYZ (same Mercator projection)
    EXPECT_EQ(6744, key.x);
}

TEST_F(TMSTileSchemeTest, TileRangeZoom1) {
    Rectangle world = Rectangle::fromDegrees(-180, -85, 180, 85);
    int minX, minY, maxX, maxY;
    tms_->tileRange(world, 1, minX, minY, maxX, maxY);

    EXPECT_EQ(0, minX);
    EXPECT_EQ(1, maxX);
    EXPECT_EQ(0, minY);
    EXPECT_EQ(1, maxY);
}

// ============================================================
// XYZ vs TMS y-axis relationship
// ============================================================

TEST_F(TMSTileSchemeTest, XyzVsTmsYRelation) {
    // 同一地理坐标在 XYZ 和 TMS 下应满足: y_tms + y_xyz = 2^z - 1
    double lngRad = 116.397 * M_PI / 180.0;
    double latRad = 39.908 * M_PI / 180.0;

    for (int z = 1; z <= 15; z += 2) {
        auto xyzKey = xyz_->positionToTile(lngRad, latRad, z);
        auto tmsKey = tms_->positionToTile(lngRad, latRad, z);

        int expectedY = (1 << z) - 1 - xyzKey.y;
        EXPECT_EQ(expectedY, tmsKey.y)
            << "At z=" << z << ": y_tms=" << tmsKey.y
            << " y_xyz=" << xyzKey.y
            << " expected y_tms=" << expectedY;
    }
}

TEST_F(TMSTileSchemeTest, SameGeographicBoundsAcrossSchemes) {
    // 同一 tile 在不同 scheme 下应该覆盖相同的地理区域
    // （只需 x 和 y 正确映射）
    double lngRad = 0.0;
    double latRad = 45.0 * M_PI / 180.0;

    for (int z = 2; z <= 8; z += 2) {
        auto xyzKey = xyz_->positionToTile(lngRad, latRad, z);
        auto tmsKey = tms_->positionToTile(lngRad, latRad, z);

        Rectangle xyzBounds = xyz_->tileToRectangle(xyzKey);
        Rectangle tmsBounds = tms_->tileToRectangle(tmsKey);

        // 两个 tile 应覆盖同一地理坐标
        EXPECT_TRUE(xyzBounds.contains(lngRad, latRad));
        EXPECT_TRUE(tmsBounds.contains(lngRad, latRad));

        // 经度范围应一致
        EXPECT_NEAR(xyzBounds.west(), tmsBounds.west(), 1e-10);
        EXPECT_NEAR(xyzBounds.east(), tmsBounds.east(), 1e-10);
        EXPECT_NEAR(xyzBounds.south(), tmsBounds.south(), 1e-10);
        EXPECT_NEAR(xyzBounds.north(), tmsBounds.north(), 1e-10);
    }
}

TEST_F(TMSTileSchemeTest, LevelResolution) {
    EXPECT_NEAR(2.0 * M_PI, tms_->levelResolution(0), 1e-6);
    EXPECT_NEAR(M_PI, tms_->levelResolution(1), 1e-6);
    EXPECT_NEAR(M_PI / 2.0, tms_->levelResolution(2), 1e-6);
}

TEST(GeographicTMSSchemeTest, CesiumGeodeticLevelZeroIsTwoByOne) {
    auto scheme = TileScheme::createGeographicTMS();

    Rectangle westRoot = scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0});
    Rectangle eastRoot = scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 1, 0});

    EXPECT_NEAR(-180.0, westRoot.westDegrees(), 1e-9);
    EXPECT_NEAR(0.0, westRoot.eastDegrees(), 1e-9);
    EXPECT_NEAR(0.0, eastRoot.westDegrees(), 1e-9);
    EXPECT_NEAR(180.0, eastRoot.eastDegrees(), 1e-9);
    EXPECT_NEAR(-90.0, westRoot.southDegrees(), 1e-9);
    EXPECT_NEAR(90.0, westRoot.northDegrees(), 1e-9);
}

TEST(GeographicTMSSchemeTest, CesiumSampleTileContainsServiceArea) {
    auto scheme = TileScheme::createGeographicTMS();
    auto key = scheme->positionToTile(
        106.508 * M_PI / 180.0, 29.617 * M_PI / 180.0, 12);

    EXPECT_EQ(12, key.z);
    EXPECT_EQ(6521, key.x);
    EXPECT_EQ(2722, key.y);

    Rectangle sample = scheme->tileToRectangle(TileKey{"Geographic-TMS", 12, 6487, 2685});
    EXPECT_TRUE(sample.contains(105.17 * M_PI / 180.0, 28.1 * M_PI / 180.0));
}
