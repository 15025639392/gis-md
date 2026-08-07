#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionRenderEntryPolicy.h"

using namespace earth_engine;

TEST(TileSelectionRenderEntryPolicyTest, PlansRenderedTileWrites) {
    TileSelectionRenderEntryPlan plan =
        TileSelectionRenderEntryPolicy::plan(
            TileSelectionRenderEntryInput{false});

    EXPECT_TRUE(plan.writeSelectionState);
    EXPECT_EQ(plan.selectionState, TileSelectionState::Rendered);
    EXPECT_TRUE(plan.writeScreenSpaceError);
    EXPECT_TRUE(plan.appendVisibleTile);
    EXPECT_FALSE(plan.queueNormalLoad);

    plan = TileSelectionRenderEntryPolicy::plan(
        TileSelectionRenderEntryInput{true});

    EXPECT_TRUE(plan.queueNormalLoad);
}

