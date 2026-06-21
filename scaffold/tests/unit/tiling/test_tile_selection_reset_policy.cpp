#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionResetPolicy.h"

using namespace earth_engine;

TEST(TileSelectionResetPolicyTest, ResetsPerFrameSelectionState) {
    const TileSelectionResetPlan plan =
        TileSelectionResetPolicy::plan(
            TileSelectionResetInput{
                TileSelectionState::RenderedAndKicked,
                true,
                false});

    EXPECT_EQ(
        plan.previousSelectionState,
        TileSelectionState::RenderedAndKicked);
    EXPECT_EQ(plan.selectionState, TileSelectionState::NotVisited);
    EXPECT_DOUBLE_EQ(plan.screenSpaceError, 0.0);
    EXPECT_FALSE(plan.inFrustum);
    EXPECT_FALSE(plan.cameraInside);
    EXPECT_FALSE(plan.ancestorMeetsSse);
}

TEST(TileSelectionResetPolicyTest, PropagatesPersistentRenderabilityInputs) {
    TileSelectionResetPlan plan =
        TileSelectionResetPolicy::plan(
            TileSelectionResetInput{
                TileSelectionState::RenderedAndKicked,
                true,
                false});

    EXPECT_TRUE(plan.surfaceDrawable);
    EXPECT_FALSE(plan.completeRenderable);
    EXPECT_FALSE(plan.renderable);

    plan = TileSelectionResetPolicy::plan(
        TileSelectionResetInput{
            TileSelectionState::Refined,
            false,
            true});

    EXPECT_FALSE(plan.surfaceDrawable);
    EXPECT_TRUE(plan.completeRenderable);
    EXPECT_TRUE(plan.renderable);
}
