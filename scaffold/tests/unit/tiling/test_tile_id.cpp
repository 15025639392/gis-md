#include <gtest/gtest.h>

#include "earth_engine/tiling/OctreeTilingScheme.h"
#include "earth_engine/tiling/TileID.h"
#include "earth_engine/tiling/TileKey.h"

#include <unordered_set>

using namespace earth_engine;

TEST(TileKeyTest, ParentMatchesCesiumNativeQuadtreeTileId) {
    // Ported from cesium-native QuadtreeTileID::getParent semantics.
    EXPECT_EQ((TileKey{"Geographic-TMS", 0, 0, 0}),
              (TileKey{"Geographic-TMS", 0, 0, 0}.parent()));
    EXPECT_EQ((TileKey{"Geographic-TMS", 0, 0, 0}),
              (TileKey{"Geographic-TMS", 1, 1, 1}.parent()));
    EXPECT_EQ((TileKey{"Geographic-TMS", 2, 3, 4}),
              (TileKey{"Geographic-TMS", 3, 7, 8}.parent()));
}

TEST(TileKeyTest, InvertedCoordinatesMatchCesiumNativeQuadtreeTileId) {
    // Cesium-native computes tileCountAtLevel - coordinate - 1.
    const TileKey key{"Geographic-TMS", 3, 5, 2};

    EXPECT_EQ(10, key.invertedX(16));
    EXPECT_EQ(5, key.invertedY(8));
}

TEST(TileKeyTest, HashKeepsQuadtreeIdentityFieldsDistinct) {
    std::unordered_set<TileKey> keys;

    keys.insert(TileKey{"Geographic-TMS", 1, 2, 3});
    keys.insert(TileKey{"WebMercator", 1, 2, 3});
    keys.insert(TileKey{"Geographic-TMS", 2, 2, 3});
    keys.insert(TileKey{"Geographic-TMS", 1, 3, 3});
    keys.insert(TileKey{"Geographic-TMS", 1, 2, 4});
    keys.insert(TileKey{"Geographic-TMS", 1, 2, 3});

    EXPECT_EQ(5u, keys.size());
    EXPECT_NE(keys.end(), keys.find(TileKey{"Geographic-TMS", 1, 2, 3}));
}

TEST(OctreeTileIDTest, DefaultAndEqualityMatchCesiumNativeHeaderSemantics) {
    // 无对应测试；cesium-native OctreeTileID defaults to level/x/y/z = 0
    // and compares all identity fields exactly.
    const OctreeTileID defaultID;
    const OctreeTileID same{0, 0, 0, 0};
    const OctreeTileID differentLevel{1, 0, 0, 0};
    const OctreeTileID differentX{0, 1, 0, 0};
    const OctreeTileID differentY{0, 0, 1, 0};
    const OctreeTileID differentZ{0, 0, 0, 1};

    EXPECT_EQ(same, defaultID);
    EXPECT_FALSE(defaultID != same);
    EXPECT_NE(defaultID, differentLevel);
    EXPECT_NE(defaultID, differentX);
    EXPECT_NE(defaultID, differentY);
    EXPECT_NE(defaultID, differentZ);
}

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
