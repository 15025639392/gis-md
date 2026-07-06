#include <gtest/gtest.h>

#include "earth_engine/tiling/TileKey.h"
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/tiling/TileScheme.h"

using namespace earth_engine;

namespace {

int invertedX(const TileScheme& scheme, const TileKey& key) {
    return key.invertedX(scheme.tileCountX(key.z));
}

int invertedY(const TileScheme& scheme, const TileKey& key) {
    return key.invertedY(scheme.tileCountY(key.z));
}

} // namespace

TEST(TileKeyQuadtreeIdAlignmentTest, RootParentReturnsItself) {
    const TileKey root{"XYZ-WebMercator", 0, 0, 0};

    EXPECT_EQ(root, root.parent());
    EXPECT_EQ(root, TilePlanBuilder::parentKey(root));
}

TEST(TileKeyQuadtreeIdAlignmentTest, ParentUsesCesiumNativeBitShiftSemantics) {
    const TileKey child{"XYZ-WebMercator", 4, 11, 6};
    const TileKey parent = child.parent();

    EXPECT_EQ("XYZ-WebMercator", parent.schemeId.str());
    EXPECT_EQ(3, parent.z);
    EXPECT_EQ(5, parent.x);
    EXPECT_EQ(3, parent.y);
    EXPECT_EQ(parent, TilePlanBuilder::parentKey(child));
}

TEST(TileKeyQuadtreeIdAlignmentTest, GeographicTmsLevelZeroHasTwoRootTiles) {
    auto scheme = TileScheme::createGeographicTMS();

    EXPECT_EQ((TileKey{"Geographic-TMS", 0, 0, 0}),
              TilePlanBuilder::parentKey(TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_EQ(1, invertedX(*scheme, TileKey{"Geographic-TMS", 0, 0, 0}));
    EXPECT_EQ(0, invertedX(*scheme, TileKey{"Geographic-TMS", 0, 1, 0}));
    EXPECT_EQ(0, invertedY(*scheme, TileKey{"Geographic-TMS", 0, 0, 0}));
}

TEST(TileKeyQuadtreeIdAlignmentTest, GeographicTmsParentPreservesCesiumNativeHalving) {
    const TileKey child{"Geographic-TMS", 3, 13, 6};
    const TileKey parent = child.parent();

    EXPECT_EQ("Geographic-TMS", parent.schemeId.str());
    EXPECT_EQ(2, parent.z);
    EXPECT_EQ(6, parent.x);
    EXPECT_EQ(3, parent.y);
    EXPECT_EQ(parent, TilePlanBuilder::parentKey(child));
}

TEST(TileKeyQuadtreeIdAlignmentTest, InvertedCoordinatesUseSchemeTileCounts) {
    auto xyz = TileScheme::createXYZWebMercator();
    auto geographic = TileScheme::createGeographicTMS();

    EXPECT_EQ(4, invertedX(*xyz, TileKey{"XYZ-WebMercator", 3, 3, 5}));
    EXPECT_EQ(2, invertedY(*xyz, TileKey{"XYZ-WebMercator", 3, 3, 5}));

    EXPECT_EQ(10, invertedX(*geographic, TileKey{"Geographic-TMS", 3, 5, 6}));
    EXPECT_EQ(1, invertedY(*geographic, TileKey{"Geographic-TMS", 3, 5, 6}));
}
