#include <gtest/gtest.h>

#include "earth_engine/core/math/AxisAlignedBox.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/OctreeTilingScheme.h"

using namespace earth_engine;

TEST(OctreeTilingSchemeTest, TileCountsUseCesiumNativeRootShiftSemantics) {
    const OctreeTilingScheme scheme(AxisAlignedBox(-10.0, -20.0, -30.0,
                                                  10.0, 20.0, 30.0),
                                    2,
                                    3,
                                    4);

    EXPECT_EQ(2, scheme.rootTilesX());
    EXPECT_EQ(3, scheme.rootTilesY());
    EXPECT_EQ(4, scheme.rootTilesZ());
    EXPECT_EQ(2, scheme.tileCountX(0));
    EXPECT_EQ(3, scheme.tileCountY(0));
    EXPECT_EQ(4, scheme.tileCountZ(0));
    EXPECT_EQ(16, scheme.tileCountX(3));
    EXPECT_EQ(24, scheme.tileCountY(3));
    EXPECT_EQ(32, scheme.tileCountZ(3));
}

TEST(OctreeTilingSchemeTest, PositionToTileMatchesCesiumNativeProjectedGrid) {
    const OctreeTilingScheme scheme(AxisAlignedBox(-10.0, -20.0, -30.0,
                                                  10.0, 20.0, 30.0),
                                    2,
                                    2,
                                    3);

    const auto tile = scheme.positionToTile(Vec3(2.5, -7.5, 11.0), 2);
    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ(2, tile->level);
    EXPECT_EQ(5, tile->x);
    EXPECT_EQ(2, tile->y);
    EXPECT_EQ(8, tile->z);
}

TEST(OctreeTilingSchemeTest, PositionToTileReturnsEmptyOutsideBox) {
    const OctreeTilingScheme scheme(AxisAlignedBox(-1.0, -1.0, -1.0,
                                                  1.0, 1.0, 1.0),
                                    1,
                                    1,
                                    1);

    EXPECT_FALSE(scheme.positionToTile(Vec3(-1.1, 0.0, 0.0), 0).has_value());
    EXPECT_FALSE(scheme.positionToTile(Vec3(1.1, 0.0, 0.0), 0).has_value());
    EXPECT_FALSE(scheme.positionToTile(Vec3(0.0, -1.1, 0.0), 0).has_value());
    EXPECT_FALSE(scheme.positionToTile(Vec3(0.0, 1.1, 0.0), 0).has_value());
    EXPECT_FALSE(scheme.positionToTile(Vec3(0.0, 0.0, -1.1), 0).has_value());
    EXPECT_FALSE(scheme.positionToTile(Vec3(0.0, 0.0, 1.1), 0).has_value());
}

TEST(OctreeTilingSchemeTest, PositionToTileClampsPositiveEdgesToFinalTile) {
    const OctreeTilingScheme scheme(AxisAlignedBox(-10.0, -20.0, -30.0,
                                                  10.0, 20.0, 30.0),
                                    2,
                                    2,
                                    3);

    const auto tile = scheme.positionToTile(Vec3(10.0, 20.0, 30.0), 2);
    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ(2, tile->level);
    EXPECT_EQ(7, tile->x);
    EXPECT_EQ(7, tile->y);
    EXPECT_EQ(11, tile->z);
}

TEST(OctreeTilingSchemeTest, PositionToTileIncludesNegativeEdgesInFirstTile) {
    const OctreeTilingScheme scheme(AxisAlignedBox(-10.0, -20.0, -30.0,
                                                  10.0, 20.0, 30.0),
                                    2,
                                    2,
                                    3);

    const auto tile = scheme.positionToTile(Vec3(-10.0, -20.0, -30.0), 2);
    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ(2, tile->level);
    EXPECT_EQ(0, tile->x);
    EXPECT_EQ(0, tile->y);
    EXPECT_EQ(0, tile->z);
}

TEST(OctreeTilingSchemeTest, TileToBoxMatchesCesiumNativeGrid) {
    const OctreeTilingScheme scheme(AxisAlignedBox(0.0, 0.0, 0.0,
                                                  8.0, 12.0, 16.0),
                                    1,
                                    1,
                                    1);

    const AxisAlignedBox box = scheme.tileToBox(OctreeTileID{2, 2, 1, 3});

    EXPECT_DOUBLE_EQ(4.0, box.minimumX());
    EXPECT_DOUBLE_EQ(3.0, box.minimumY());
    EXPECT_DOUBLE_EQ(12.0, box.minimumZ());
    EXPECT_DOUBLE_EQ(6.0, box.maximumX());
    EXPECT_DOUBLE_EQ(6.0, box.maximumY());
    EXPECT_DOUBLE_EQ(16.0, box.maximumZ());
}

TEST(OctreeTilingSchemeTest, TileToBoxUsesCesiumNativeOriginBasedOffsets) {
    const OctreeTilingScheme scheme(AxisAlignedBox(100.0, 200.0, 300.0,
                                                  108.0, 212.0, 316.0),
                                    1,
                                    1,
                                    1);

    const AxisAlignedBox box = scheme.tileToBox(OctreeTileID{2, 2, 1, 3});

    EXPECT_DOUBLE_EQ(4.0, box.minimumX());
    EXPECT_DOUBLE_EQ(3.0, box.minimumY());
    EXPECT_DOUBLE_EQ(12.0, box.minimumZ());
    EXPECT_DOUBLE_EQ(6.0, box.maximumX());
    EXPECT_DOUBLE_EQ(6.0, box.maximumY());
    EXPECT_DOUBLE_EQ(16.0, box.maximumZ());
}

TEST(OctreeTilingSchemeTest, TileToBoxAllowsOutOfRangeIdsLikeCesiumNative) {
    const OctreeTilingScheme scheme(AxisAlignedBox(0.0, 0.0, 0.0,
                                                  4.0, 4.0, 4.0),
                                    1,
                                    1,
                                    1);

    const AxisAlignedBox box = scheme.tileToBox(OctreeTileID{1, 3, 2, 4});

    EXPECT_DOUBLE_EQ(6.0, box.minimumX());
    EXPECT_DOUBLE_EQ(4.0, box.minimumY());
    EXPECT_DOUBLE_EQ(8.0, box.minimumZ());
    EXPECT_DOUBLE_EQ(8.0, box.maximumX());
    EXPECT_DOUBLE_EQ(6.0, box.maximumY());
    EXPECT_DOUBLE_EQ(10.0, box.maximumZ());
}
