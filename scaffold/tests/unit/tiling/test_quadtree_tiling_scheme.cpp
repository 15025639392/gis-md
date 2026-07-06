#include <gtest/gtest.h>

#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/QuadtreeTilingScheme.h"
#include "earth_engine/tiling/TileKey.h"

using namespace earth_engine;

TEST(QuadtreeTilingSchemeTest, TileCountsUseCesiumNativeRootShiftSemantics) {
    const QuadtreeTilingScheme scheme(Rectangle(-180.0, -90.0, 180.0, 90.0),
                                      2,
                                      1);

    EXPECT_EQ(2, scheme.rootTilesX());
    EXPECT_EQ(1, scheme.rootTilesY());
    EXPECT_EQ(2, scheme.tileCountX(0));
    EXPECT_EQ(1, scheme.tileCountY(0));
    EXPECT_EQ(16, scheme.tileCountX(3));
    EXPECT_EQ(8, scheme.tileCountY(3));
}

TEST(QuadtreeTilingSchemeTest, PositionToTileMatchesCesiumNativeProjectedGrid) {
    const QuadtreeTilingScheme scheme(Rectangle(-180.0, -90.0, 180.0, 90.0),
                                      2,
                                      1);

    const auto tile = scheme.positionToTile(106.508, 29.617, 12);
    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ("", tile->schemeId.str());
    EXPECT_EQ(12, tile->z);
    EXPECT_EQ(6519, tile->x);
    EXPECT_EQ(2721, tile->y);
}

TEST(QuadtreeTilingSchemeTest, PositionToTilePreservesLocalSchemeIdentity) {
    const QuadtreeTilingScheme scheme(Rectangle(-180.0, -90.0, 180.0, 90.0),
                                      2,
                                      1,
                                      "Geographic-TMS");

    const auto tile = scheme.positionToTile(0.0, 0.0, 0);

    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ("Geographic-TMS", tile->schemeId.str());
    EXPECT_EQ(0, tile->z);
    EXPECT_EQ(1, tile->x);
    EXPECT_EQ(0, tile->y);
}

TEST(QuadtreeTilingSchemeTest, PositionToTileReturnsEmptyOutsideRectangle) {
    const QuadtreeTilingScheme scheme(Rectangle(-10.0, -5.0, 10.0, 5.0),
                                      1,
                                      1);

    EXPECT_FALSE(scheme.positionToTile(-10.1, 0.0, 0).has_value());
    EXPECT_FALSE(scheme.positionToTile(10.1, 0.0, 0).has_value());
    EXPECT_FALSE(scheme.positionToTile(0.0, -5.1, 0).has_value());
    EXPECT_FALSE(scheme.positionToTile(0.0, 5.1, 0).has_value());
}

TEST(QuadtreeTilingSchemeTest, PositionToTileClampsPositiveEdgesToFinalTile) {
    const QuadtreeTilingScheme scheme(Rectangle(-180.0, -90.0, 180.0, 90.0),
                                      2,
                                      1);

    const auto tile = scheme.positionToTile(180.0, 90.0, 3);
    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ(3, tile->z);
    EXPECT_EQ(15, tile->x);
    EXPECT_EQ(7, tile->y);
}

TEST(QuadtreeTilingSchemeTest, PositionToTileAssignsInternalBoundariesToUpperTile) {
    // Source-derived from cesium-native QuadtreeTilingScheme::positionToTile:
    // tile coordinates are computed with integer truncation, so exact internal
    // boundaries fall into the tile on the positive side of the boundary.
    const QuadtreeTilingScheme scheme(Rectangle(0.0, 0.0, 4.0, 4.0), 1, 1);

    const auto tile = scheme.positionToTile(2.0, 2.0, 1);

    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ(1, tile->z);
    EXPECT_EQ(1, tile->x);
    EXPECT_EQ(1, tile->y);
}

TEST(QuadtreeTilingSchemeTest, PositionToTileIncludesNegativeEdgesInFirstTile) {
    const QuadtreeTilingScheme scheme(Rectangle(-180.0, -90.0, 180.0, 90.0),
                                      2,
                                      1);

    const auto tile = scheme.positionToTile(-180.0, -90.0, 3);
    ASSERT_TRUE(tile.has_value());
    EXPECT_EQ(3, tile->z);
    EXPECT_EQ(0, tile->x);
    EXPECT_EQ(0, tile->y);
}

TEST(QuadtreeTilingSchemeTest, TileToRectangleMatchesCesiumNativeGrid) {
    const QuadtreeTilingScheme scheme(Rectangle(-180.0, -90.0, 180.0, 90.0),
                                      2,
                                      1);

    const Rectangle rectangle = scheme.tileToRectangle(TileKey{"", 3, 10, 5});

    EXPECT_NEAR(45.0, rectangle.west(), 1e-14);
    EXPECT_NEAR(22.5, rectangle.south(), 1e-14);
    EXPECT_NEAR(67.5, rectangle.east(), 1e-14);
    EXPECT_NEAR(45.0, rectangle.north(), 1e-14);
}

TEST(QuadtreeTilingSchemeTest, TileToRectangleAllowsOutOfRangeIdsLikeCesiumNative) {
    const QuadtreeTilingScheme scheme(Rectangle(0.0, 0.0, 4.0, 4.0), 1, 1);

    const Rectangle rectangle = scheme.tileToRectangle(TileKey{"", 1, 3, 2});

    EXPECT_DOUBLE_EQ(6.0, rectangle.west());
    EXPECT_DOUBLE_EQ(4.0, rectangle.south());
    EXPECT_DOUBLE_EQ(8.0, rectangle.east());
    EXPECT_DOUBLE_EQ(6.0, rectangle.north());
}
