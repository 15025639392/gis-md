#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionCullingPolicy.h"

using namespace earth_engine;

TEST(TileSelectionCullingPolicyTest, ReplaceTilesUseChildrenBoundsOnlyWhenSafe) {
    EXPECT_TRUE(TileSelectionCullingPolicy::shouldUseChildrenBounds(
        TileRefine::Replace,
        true,
        false));
    EXPECT_FALSE(TileSelectionCullingPolicy::shouldUseChildrenBounds(
        TileRefine::Add,
        true,
        false));
    EXPECT_FALSE(TileSelectionCullingPolicy::shouldUseChildrenBounds(
        TileRefine::Replace,
        false,
        false));
    EXPECT_FALSE(TileSelectionCullingPolicy::shouldUseChildrenBounds(
        TileRefine::Replace,
        true,
        true));
}

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

TEST(TileSelectionCullingPolicyTest, CulledTileLoadPlanMatchesNativeOptions) {
    TileSelectionCullLoadPlan plan =
        TileSelectionCullingPolicy::planCulledTileLoad(
            false,
            false,
            TileRefine::Replace);
    EXPECT_FALSE(plan.queueLoad);

    plan = TileSelectionCullingPolicy::planCulledTileLoad(
        true,
        false,
        TileRefine::Replace);
    EXPECT_TRUE(plan.queueLoad);
    EXPECT_EQ(plan.group, TileLoadPriorityGroup::Preload);

    plan = TileSelectionCullingPolicy::planCulledTileLoad(
        false,
        true,
        TileRefine::Add);
    EXPECT_FALSE(plan.queueLoad);

    plan = TileSelectionCullingPolicy::planCulledTileLoad(
        false,
        true,
        TileRefine::Replace);
    EXPECT_TRUE(plan.queueLoad);
    EXPECT_EQ(plan.group, TileLoadPriorityGroup::Normal);

    plan = TileSelectionCullingPolicy::planCulledTileLoad(
        true,
        true,
        TileRefine::Replace);
    EXPECT_TRUE(plan.queueLoad);
    EXPECT_EQ(plan.group, TileLoadPriorityGroup::Normal);
}
