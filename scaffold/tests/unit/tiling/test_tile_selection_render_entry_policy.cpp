#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionRenderEntryPolicy.h"

using namespace earth_engine;

TEST(TileSelectionRenderEntryPolicyTest, PlansRenderedTileWrites) {
    TileSelectionRenderEntryPlan plan =
        TileSelectionRenderEntryPolicy::plan(
            TileSelectionRenderEntryInput{true, false});

    EXPECT_TRUE(plan.writeSelectionState);
    EXPECT_EQ(plan.selectionState, TileSelectionState::Rendered);
    EXPECT_TRUE(plan.writeScreenSpaceError);
    EXPECT_TRUE(plan.appendVisibleTile);
    EXPECT_FALSE(plan.resetLodTransitionFade);
    EXPECT_FALSE(plan.queueNormalLoad);

    plan = TileSelectionRenderEntryPolicy::plan(
        TileSelectionRenderEntryInput{false, true});

    EXPECT_TRUE(plan.resetLodTransitionFade);
    EXPECT_FLOAT_EQ(plan.lodTransitionFadeValue, 1.0f);
    EXPECT_TRUE(plan.queueNormalLoad);
}

