#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionKickPolicy.h"
#include "earth_engine/tiling/TileSelectionPreTraversalPolicy.h"

using namespace earth_engine;

TEST(
    TileSelectionPreTraversalPolicyTest,
    AddRefinementRendersParentBeforeVisitingChildren) {
    TileSelectionRefineFlowResult refineFlow;
    refineFlow.refine = true;
    refineFlow.ancestorMeetsSse = false;

    const TileSelectionPreTraversalPlan plan =
        TileSelectionPreTraversalPolicy::plan(
            TileSelectionPreTraversalInput{
                true,
                TileRefine::Add,
                refineFlow});

    EXPECT_FALSE(plan.finishAsSingleTile);
    EXPECT_TRUE(plan.visitChildren);
    EXPECT_TRUE(plan.addAdditiveParentToPlan);
    EXPECT_TRUE(plan.additiveParentShouldQueueLoad);
    EXPECT_TRUE(plan.queuedForLoadAfterPreTraversal);
    EXPECT_FALSE(plan.ancestorMeetsSseAfterPreTraversal);
}

TEST(
    TileSelectionPreTraversalPolicyTest,
    AddRefinementDoesNotDuplicateAlreadyQueuedParentLoad) {
    TileSelectionRefineFlowResult refineFlow;
    refineFlow.refine = true;
    refineFlow.queuedForLoad = true;
    refineFlow.queueUrgentLoad = true;
    refineFlow.ancestorMeetsSse = true;

    const TileSelectionPreTraversalPlan plan =
        TileSelectionPreTraversalPolicy::plan(
            TileSelectionPreTraversalInput{
                true,
                TileRefine::Add,
                refineFlow});

    EXPECT_TRUE(plan.queueUrgentLoad);
    EXPECT_TRUE(plan.visitChildren);
    EXPECT_TRUE(plan.addAdditiveParentToPlan);
    EXPECT_FALSE(plan.additiveParentShouldQueueLoad);
    EXPECT_TRUE(plan.queuedForLoadAfterPreTraversal);
    EXPECT_TRUE(plan.ancestorMeetsSseAfterPreTraversal);
}

TEST(
    TileSelectionKickPolicyTest,
    AddRefinementDoesNotReaddParentAsReplacementAfterKick) {
    TileTraversalDetails manyMissing;
    manyMissing.allAreRenderable = false;
    manyMissing.anyWereRenderedLastFrame = false;
    manyMissing.notYetRenderableCount = 3;

    const TileSelectionKickPlan plan =
        TileSelectionKickPolicy::planAfterKick(
            manyMissing,
            false,
            2,
            false,
            false,
            TileRefine::Add,
            true,
            false,
            true);

    EXPECT_TRUE(plan.restoreChildLoadQueueAndLoadParent);
    EXPECT_FALSE(plan.addRenderableReplacementToPlan);
    EXPECT_FALSE(plan.preloadParent);
}

TEST(
    TileSelectionKickPolicyTest,
    ReplaceRefinementAddsRenderableParentReplacementAfterKick) {
    TileTraversalDetails manyMissing;
    manyMissing.allAreRenderable = false;
    manyMissing.anyWereRenderedLastFrame = false;
    manyMissing.notYetRenderableCount = 3;

    const TileSelectionKickPlan plan =
        TileSelectionKickPolicy::planAfterKick(
            manyMissing,
            false,
            2,
            false,
            false,
            TileRefine::Replace,
            true,
            false,
            true);

    EXPECT_TRUE(plan.restoreChildLoadQueueAndLoadParent);
    EXPECT_TRUE(plan.addRenderableReplacementToPlan);
    EXPECT_FALSE(plan.preloadParent);
}
