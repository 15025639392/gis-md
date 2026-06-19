#include <gtest/gtest.h>

#include "earth_engine/tiling/OctreeTilingScheme.h"
#include "earth_engine/tiling/TileID.h"
#include "earth_engine/tiling/TileKey.h"

using namespace earth_engine;

TEST(TileIdUtilitiesTest, CreatesStringForExplicitContentUrl) {
    const TileID tileID = std::string("tiles/0/0/0.b3dm");

    EXPECT_EQ("tiles/0/0/0.b3dm",
              TileIdUtilities::createTileIdString(tileID));
}

TEST(TileIdUtilitiesTest, CreatesStringForQuadtreeTileId) {
    const TileID tileID = TileKey{"Geographic-TMS", 10, 23, 144};

    EXPECT_EQ("L10-X23-Y144",
              TileIdUtilities::createTileIdString(tileID));
}

TEST(TileIdUtilitiesTest, CreatesStringForOctreeTileId) {
    const TileID tileID = OctreeTileID{10, 23, 144, 42};

    EXPECT_EQ("L10-X23-Y144-Z42",
              TileIdUtilities::createTileIdString(tileID));
}

TEST(TileIdUtilitiesTest, CreatesStringForUpsampledQuadtreeNode) {
    const TileID tileID =
        UpsampledQuadtreeNode{TileKey{"Geographic-TMS", 10, 23, 144}};

    EXPECT_EQ("upsampled-L10-X23-Y144",
              TileIdUtilities::createTileIdString(tileID));
}

TEST(TileIdUtilitiesTest, BlankStringIsTheOnlyNonLoadableId) {
    EXPECT_FALSE(TileIdUtilities::isLoadable(TileID{std::string()}));
    EXPECT_TRUE(TileIdUtilities::isLoadable(TileID{std::string("0/0/0.b3dm")}));
    EXPECT_TRUE(TileIdUtilities::isLoadable(TileID{TileKey{"", 0, 0, 0}}));
    EXPECT_TRUE(TileIdUtilities::isLoadable(TileID{OctreeTileID{0, 0, 0, 0}}));
    EXPECT_TRUE(TileIdUtilities::isLoadable(
        TileID{UpsampledQuadtreeNode{TileKey{"", 1, 1, 0}}}));
}
