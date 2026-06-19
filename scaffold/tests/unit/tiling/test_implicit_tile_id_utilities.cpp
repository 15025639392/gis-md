#include <gtest/gtest.h>

#include "earth_engine/tiling/ImplicitTileIdUtilities.h"

#include <vector>

using namespace earth_engine;

TEST(ImplicitTileIdUtilitiesTest, QuadtreeChildrenMatchCesiumNativeOrder) {
    const TileKey parent{"Geographic-TMS", 11, 2, 3};

    const std::vector<TileKey> children =
        ImplicitTileIdUtilities::children(parent);

    const std::vector<TileKey> expected{
        TileKey{"Geographic-TMS", 12, 4, 6},
        TileKey{"Geographic-TMS", 12, 5, 6},
        TileKey{"Geographic-TMS", 12, 4, 7},
        TileKey{"Geographic-TMS", 12, 5, 7}};
    EXPECT_EQ(expected, children);
}

TEST(ImplicitTileIdUtilitiesTest, OctreeChildrenMatchCesiumNativeOrder) {
    const OctreeTileID parent{11, 2, 3, 4};

    const std::vector<OctreeTileID> children =
        ImplicitTileIdUtilities::children(parent);

    const std::vector<OctreeTileID> expected{
        OctreeTileID{12, 4, 6, 8},
        OctreeTileID{12, 5, 6, 8},
        OctreeTileID{12, 4, 7, 8},
        OctreeTileID{12, 5, 7, 8},
        OctreeTileID{12, 4, 6, 9},
        OctreeTileID{12, 5, 6, 9},
        OctreeTileID{12, 4, 7, 9},
        OctreeTileID{12, 5, 7, 9}};
    EXPECT_EQ(expected, children);
}

TEST(ImplicitTileIdUtilitiesTest, ResolveUrlMatchesCesiumNative) {
    EXPECT_EQ("https://example.com/tiles/11/2/3",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com",
                  "tiles/{level}/{x}/{y}",
                  TileKey{"Geographic-TMS", 11, 2, 3}));

    EXPECT_EQ("https://example.com/tiles/11/2/3/4",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com",
                  "tiles/{level}/{x}/{y}/{z}",
                  OctreeTileID{11, 2, 3, 4}));
}

TEST(ImplicitTileIdUtilitiesTest, ResolveUrlPreservesTemplateEdgeCases) {
    EXPECT_EQ("https://example.com/base/tiles/11/unknown/3",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com/base/tileset.json",
                  "tiles/{level}/{unknown}/{y}",
                  TileKey{"Geographic-TMS", 11, 2, 3}));

    EXPECT_EQ("https://example.com/tiles/{level",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com/base/tileset.json",
                  "../tiles/{level",
                  TileKey{"Geographic-TMS", 11, 2, 3}));

    EXPECT_EQ("https://example.com/base/tileset.json",
              ImplicitTileIdUtilities::resolveUrl(
                  "https://example.com/base/tileset.json?token=base#section",
                  "",
                  TileKey{"Geographic-TMS", 11, 2, 3}));
}

TEST(ImplicitTileIdUtilitiesTest, ParentIdMatchesCesiumNativeOptionalSemantics) {
    const std::optional<TileKey> quadtreeParent =
        ImplicitTileIdUtilities::parentId(TileKey{"Geographic-TMS", 2, 1, 2});
    ASSERT_TRUE(quadtreeParent.has_value());
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 0, 1}), *quadtreeParent);
    EXPECT_FALSE(
        ImplicitTileIdUtilities::parentId(
            TileKey{"Geographic-TMS", 0, 0, 0})
            .has_value());

    const std::optional<OctreeTileID> octreeParent =
        ImplicitTileIdUtilities::parentId(OctreeTileID{2, 3, 1, 2});
    ASSERT_TRUE(octreeParent.has_value());
    EXPECT_EQ((OctreeTileID{1, 1, 0, 1}), *octreeParent);
    EXPECT_FALSE(
        ImplicitTileIdUtilities::parentId(OctreeTileID{0, 0, 0, 0})
            .has_value());
}

TEST(ImplicitTileIdUtilitiesTest, SubtreeRootIdMatchesCesiumNative) {
    EXPECT_EQ((TileKey{"Geographic-TMS", 10, 2, 3}),
              ImplicitTileIdUtilities::subtreeRootId(
                  5,
                  TileKey{"Geographic-TMS", 10, 2, 3}));
    EXPECT_EQ((TileKey{"Geographic-TMS", 8, 0, 0}),
              ImplicitTileIdUtilities::subtreeRootId(
                  4,
                  TileKey{"Geographic-TMS", 10, 2, 3}));

    EXPECT_EQ((OctreeTileID{10, 2, 3, 4}),
              ImplicitTileIdUtilities::subtreeRootId(
                  5,
                  OctreeTileID{10, 2, 3, 4}));
    EXPECT_EQ((OctreeTileID{8, 0, 0, 1}),
              ImplicitTileIdUtilities::subtreeRootId(
                  4,
                  OctreeTileID{10, 2, 3, 4}));
}

TEST(ImplicitTileIdUtilitiesTest, AbsoluteTileIdToRelativeMatchesCesiumNative) {
    EXPECT_EQ((TileKey{"Geographic-TMS", 11, 2, 3}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  TileKey{"Geographic-TMS", 0, 0, 0},
                  TileKey{"Geographic-TMS", 11, 2, 3}));
    EXPECT_EQ((TileKey{"Geographic-TMS", 0, 0, 0}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  TileKey{"Geographic-TMS", 11, 2, 3},
                  TileKey{"Geographic-TMS", 11, 2, 3}));
    EXPECT_EQ((TileKey{"Geographic-TMS", 1, 1, 1}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  TileKey{"Geographic-TMS", 11, 2, 3},
                  TileKey{"Geographic-TMS", 12, 5, 7}));

    EXPECT_EQ((OctreeTileID{11, 2, 3, 4}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  OctreeTileID{0, 0, 0, 0},
                  OctreeTileID{11, 2, 3, 4}));
    EXPECT_EQ((OctreeTileID{0, 0, 0, 0}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  OctreeTileID{11, 2, 3, 4},
                  OctreeTileID{11, 2, 3, 4}));
    EXPECT_EQ((OctreeTileID{1, 1, 1, 1}),
              ImplicitTileIdUtilities::absoluteTileIdToRelative(
                  OctreeTileID{11, 2, 3, 4},
                  OctreeTileID{12, 5, 7, 9}));
}

TEST(ImplicitTileIdUtilitiesTest, MortonIndexMatchesCesiumNativeExamples) {
    EXPECT_EQ(14ULL,
              ImplicitTileIdUtilities::mortonIndex(
                  TileKey{"Geographic-TMS", 11, 2, 3}));
    EXPECT_EQ(282ULL,
              ImplicitTileIdUtilities::mortonIndex(
                  OctreeTileID{11, 2, 3, 4}));

    EXPECT_EQ(1ULL,
              ImplicitTileIdUtilities::relativeMortonIndex(
                  TileKey{"Geographic-TMS", 11, 2, 3},
                  TileKey{"Geographic-TMS", 12, 5, 6}));
    EXPECT_EQ(1ULL,
              ImplicitTileIdUtilities::relativeMortonIndex(
                  OctreeTileID{11, 2, 3, 4},
                  OctreeTileID{12, 5, 6, 8}));
}

TEST(ImplicitTileIdUtilitiesTest, LevelDenominatorMatchesCesiumNative) {
    EXPECT_DOUBLE_EQ(1.0, ImplicitTileIdUtilities::levelDenominator(0));
    EXPECT_DOUBLE_EQ(2.0, ImplicitTileIdUtilities::levelDenominator(1));
    EXPECT_DOUBLE_EQ(4.0, ImplicitTileIdUtilities::levelDenominator(2));
}
