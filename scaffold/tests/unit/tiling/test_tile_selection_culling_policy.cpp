#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionCullingPolicy.h"

using namespace earth_engine;

TEST(TileSelectionCullingPolicyTest, VisibleTilesUseStrictMaximumSse) {
    EXPECT_TRUE(TileSelectionCullingPolicy::meetsScreenSpaceError(
        false,
        15.999,
        16.0,
        true,
        8.0));
    EXPECT_FALSE(TileSelectionCullingPolicy::meetsScreenSpaceError(
        false,
        16.0,
        16.0,
        true,
        8.0));
}

TEST(TileSelectionCullingPolicyTest, CulledTilesUseCesiumCulledSseOption) {
    EXPECT_TRUE(TileSelectionCullingPolicy::meetsScreenSpaceError(
        true,
        1000.0,
        16.0,
        false,
        8.0));

    EXPECT_TRUE(TileSelectionCullingPolicy::meetsScreenSpaceError(
        true,
        7.999,
        16.0,
        true,
        8.0));
    EXPECT_FALSE(TileSelectionCullingPolicy::meetsScreenSpaceError(
        true,
        8.0,
        16.0,
        true,
        8.0));
}

