#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionKickPolicy.h"
#include "earth_engine/tiling/TileSelectionPostTraversalPolicy.h"
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

TEST(
    TileSelectionPostTraversalPolicyTest,
    ReplaceKickRestoresChildrenAndQueuesRenderableParent) {
    TileTraversalDetails manyMissing;
    manyMissing.allAreRenderable = false;
    manyMissing.anyWereRenderedLastFrame = false;
    manyMissing.notYetRenderableCount = 3;

    const TileSelectionPostTraversalResult result =
        TileSelectionPostTraversalPolicy::evaluate(
            TileSelectionPostTraversalInput{
                manyMissing,
                true,
                false,
                TileSelectionState::NotVisited,
                false,
                1.0f,
                false,
                false,
                TileRefine::Replace,
                false},
            TileSelectionPostTraversalOptions{
                2,
                false,
                false,
                true});

    EXPECT_TRUE(result.shouldKick);
    EXPECT_FALSE(result.wasReallyRenderedLastFrame);
    EXPECT_TRUE(result.kickPlan.restoreChildLoadQueueAndLoadParent);
    EXPECT_TRUE(result.kickPlan.addRenderableReplacementToPlan);
    EXPECT_FALSE(result.kickPlan.preloadParent);
    EXPECT_FALSE(result.preloadRefinedAncestor);

    const TileSelectionPostTraversalCommitPlan plan =
        TileSelectionPostTraversalPolicy::commitPlan(result, false);

    EXPECT_TRUE(plan.kickVisitedDescendants);
    EXPECT_TRUE(plan.trimRenderedDescendants);
    EXPECT_TRUE(plan.restoreChildLoadQueue);
    EXPECT_TRUE(plan.queueParentNormal);
    EXPECT_TRUE(plan.addRenderableReplacementToPlan);
    EXPECT_TRUE(plan.returnSingleTileDetails);
    EXPECT_FALSE(plan.markTileRefined);
}

TEST(
    TileSelectionPostTraversalPolicyTest,
    QueuedParentKickDoesNotDuplicateNormalLoad) {
    TileSelectionPostTraversalResult result;
    result.shouldKick = true;
    result.kickPlan.restoreChildLoadQueueAndLoadParent = true;

    const TileSelectionPostTraversalCommitPlan plan =
        TileSelectionPostTraversalPolicy::commitPlan(result, true);

    EXPECT_TRUE(plan.restoreChildLoadQueue);
    EXPECT_FALSE(plan.queueParentNormal);
}
