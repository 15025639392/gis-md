#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileSelectionKickPolicy.h"
#include "earth_engine/tiling/TileSelectionPostTraversalCommitter.h"
#include "earth_engine/tiling/TileSelectionPostTraversalPolicy.h"
#include "earth_engine/tiling/TileSelectionPreTraversalPolicy.h"
#include "earth_engine/tiling/TileSelectionRefineFlowPolicy.h"
#include "earth_engine/tiling/TileSelectionRefinementPolicy.h"

#include <vector>

using namespace earth_engine;

TEST(
    TileSelectionPreTraversalPolicyTest,
    CannotRefineFinishesAsLoadQueuedSingleTile) {
    TileSelectionRefineFlowResult refineFlow;
    refineFlow.meetsScreenSpaceError = false;
    refineFlow.ancestorMeetsSse = true;

    const TileSelectionPreTraversalPlan plan =
        TileSelectionPreTraversalPolicy::plan(
            TileSelectionPreTraversalInput{
                false,
                TileRefine::Replace,
                refineFlow});

    EXPECT_TRUE(plan.finishAsSingleTile);
    EXPECT_EQ(
        plan.finishReason,
        TileSelectionPreTraversalFinishReason::CannotRefine);
    EXPECT_TRUE(plan.singleTileShouldQueueLoad);
    EXPECT_FALSE(plan.visitChildren);
}

TEST(
    TileSelectionPreTraversalPolicyTest,
    SatisfiedSseFinishesWithoutDuplicateAncestorMeetLoad) {
    TileSelectionRefineFlowResult refineFlow;
    refineFlow.refine = false;
    refineFlow.meetsScreenSpaceError = true;
    refineFlow.ancestorMeetsSse = true;

    const TileSelectionPreTraversalPlan plan =
        TileSelectionPreTraversalPolicy::plan(
            TileSelectionPreTraversalInput{
                true,
                TileRefine::Replace,
                refineFlow});

    EXPECT_TRUE(plan.finishAsSingleTile);
    EXPECT_EQ(
        plan.finishReason,
        TileSelectionPreTraversalFinishReason::DoesNotRefine);
    EXPECT_FALSE(plan.singleTileShouldQueueLoad);
    EXPECT_FALSE(plan.visitChildren);
}

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
    DoesNotKickUnrenderableParentWithinLoadingDescendantLimit) {
    TileTraversalDetails oneMissing;
    oneMissing.allAreRenderable = false;
    oneMissing.anyWereRenderedLastFrame = false;
    oneMissing.notYetRenderableCount = 1;

    EXPECT_FALSE(TileSelectionKickPolicy::shouldKickDescendants(
        oneMissing,
        false,
        false,
        1,
        false,
        false,
        TileSelectionState::NotVisited,
        false,
        1.0f));
}

TEST(
    TileSelectionKickPolicyTest,
    KicksUnrenderableParentAfterLoadingDescendantLimit) {
    TileTraversalDetails manyMissing;
    manyMissing.allAreRenderable = false;
    manyMissing.anyWereRenderedLastFrame = false;
    manyMissing.notYetRenderableCount = 2;

    EXPECT_TRUE(TileSelectionKickPolicy::shouldKickDescendants(
        manyMissing,
        false,
        false,
        1,
        false,
        false,
        TileSelectionState::NotVisited,
        false,
        1.0f));
}

TEST(
    TileSelectionKickPolicyTest,
    FadingInRenderedTileCanKickDescendants) {
    TileTraversalDetails fadingDescendants;
    fadingDescendants.allAreRenderable = true;
    fadingDescendants.anyWereRenderedLastFrame = true;
    fadingDescendants.notYetRenderableCount = 0;

    EXPECT_TRUE(TileSelectionKickPolicy::shouldKickDescendants(
        fadingDescendants,
        true,
        false,
        20,
        true,
        true,
        TileSelectionState::Rendered,
        true,
        0.5f));

    EXPECT_FALSE(TileSelectionKickPolicy::shouldKickDescendants(
        fadingDescendants,
        true,
        false,
        20,
        true,
        true,
        TileSelectionState::Rendered,
        true,
        1.0f));
}

TEST(
    TileSelectionKickPolicyTest,
    KickedRenderedStateDoesNotTriggerFadeInKick) {
    TileTraversalDetails fadingDescendants;
    fadingDescendants.allAreRenderable = true;
    fadingDescendants.anyWereRenderedLastFrame = true;
    fadingDescendants.notYetRenderableCount = 0;

    EXPECT_FALSE(TileSelectionKickPolicy::shouldKickDescendants(
        fadingDescendants,
        true,
        false,
        20,
        true,
        true,
        TileSelectionState::RenderedAndKicked,
        true,
        0.5f));
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
    TileSelectionKickPolicyTest,
    RestoresChildQueueOnlyWhenParentShouldReplaceDescendantLoads) {
    TileTraversalDetails tooManyMissing;
    tooManyMissing.allAreRenderable = false;
    tooManyMissing.anyWereRenderedLastFrame = false;
    tooManyMissing.notYetRenderableCount = 3;

    EXPECT_TRUE(TileSelectionKickPolicy::shouldRestoreChildLoadQueueAndLoadParent(
        tooManyMissing,
        false,
        2,
        false,
        false));
    EXPECT_FALSE(TileSelectionKickPolicy::shouldRestoreChildLoadQueueAndLoadParent(
        tooManyMissing,
        true,
        2,
        false,
        false));
    EXPECT_FALSE(TileSelectionKickPolicy::shouldRestoreChildLoadQueueAndLoadParent(
        tooManyMissing,
        false,
        3,
        false,
        false));
    EXPECT_FALSE(TileSelectionKickPolicy::shouldRestoreChildLoadQueueAndLoadParent(
        tooManyMissing,
        false,
        2,
        true,
        false));
    EXPECT_FALSE(TileSelectionKickPolicy::shouldRestoreChildLoadQueueAndLoadParent(
        tooManyMissing,
        false,
        2,
        false,
        true));
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

TEST(
    TileSelectionPostTraversalPolicyTest,
    RefinedAncestorPreloadsWhenEnabledAndNotAlreadyQueued) {
    const TileTraversalDetails ready =
        TileTraversalDetailsPolicy::forSingleTile(true, false);

    const TileSelectionPostTraversalResult result =
        TileSelectionPostTraversalPolicy::evaluate(
            TileSelectionPostTraversalInput{
                ready,
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

    EXPECT_FALSE(result.shouldKick);
    EXPECT_TRUE(result.preloadRefinedAncestor);

    const TileSelectionPostTraversalCommitPlan plan =
        TileSelectionPostTraversalPolicy::commitPlan(result, false);

    EXPECT_TRUE(plan.markTileRefined);
    EXPECT_TRUE(plan.queueParentPreload);
    EXPECT_FALSE(plan.returnSingleTileDetails);
}

TEST(
    TileSelectionPostTraversalPolicyTest,
    QueuedRefinedAncestorSkipsPreloadDuplicate) {
    const TileTraversalDetails ready =
        TileTraversalDetailsPolicy::forSingleTile(true, false);

    const TileSelectionPostTraversalResult result =
        TileSelectionPostTraversalPolicy::evaluate(
            TileSelectionPostTraversalInput{
                ready,
                true,
                false,
                TileSelectionState::NotVisited,
                false,
                1.0f,
                false,
                false,
                TileRefine::Replace,
                true},
            TileSelectionPostTraversalOptions{
                2,
                false,
                false,
                true});

    EXPECT_FALSE(result.shouldKick);
    EXPECT_FALSE(result.preloadRefinedAncestor);
}

TEST(
    TileSelectionPostTraversalPolicyTest,
    FadingAddKickAvoidsDuplicateParentLoad) {
    const TileTraversalDetails ready =
        TileTraversalDetailsPolicy::forSingleTile(true, true);

    const TileSelectionPostTraversalResult result =
        TileSelectionPostTraversalPolicy::evaluate(
            TileSelectionPostTraversalInput{
                ready,
                true,
                false,
                TileSelectionState::Rendered,
                true,
                0.5f,
                true,
                false,
                TileRefine::Add,
                true},
            TileSelectionPostTraversalOptions{
                20,
                true,
                true,
                true});

    EXPECT_TRUE(result.shouldKick);
    EXPECT_TRUE(result.wasReallyRenderedLastFrame);
    EXPECT_FALSE(result.kickPlan.restoreChildLoadQueueAndLoadParent);
    EXPECT_FALSE(result.kickPlan.addRenderableReplacementToPlan);
    EXPECT_FALSE(result.kickPlan.preloadParent);
}

TEST(
    TileSelectionPostTraversalCommitterTest,
    KickTrimsDescendantsRestoresQueueAndReturnsParentDetails) {
    TilesetTile parent(TileKey{"test", 0, 0, 0}, Rectangle{});
    TilesetTile child(TileKey{"test", 1, 0, 0}, Rectangle{}, &parent);
    parent.children.push_back(&child);

    TilePlan tilePlan;
    tilePlan.visibleTiles.push_back(parent.key);
    tilePlan.visibleTiles.push_back(child.key);

    TileLoadQueue loadQueue;
    loadQueue.queue(child.key, TileLoadPriorityGroup::Normal, 2.0);

    TileSelectionCounters counters;
    TileSelectionPostTraversalCommitPlan plan;
    plan.kickVisitedDescendants = true;
    plan.trimRenderedDescendants = true;
    plan.returnSingleTileDetails = true;
    plan.restoreChildLoadQueue = true;
    plan.queueParentNormal = true;
    plan.wasReallyRenderedLastFrame = false;

    bool kickedParent = false;
    std::vector<TileLoadRequest> queuedRequests;
    const TileSelectionPostTraversalCommitResult result =
        TileSelectionPostTraversalCommitter::commit(
            parent,
            tilePlan,
            loadQueue,
            counters,
            plan,
            TileSelectionPostTraversalCommitContext{
                1,
                0,
                4.0,
                5.0,
                false},
            [&kickedParent](TilesetTile& kickedTile) {
                kickedParent = kickedTile.key.z == 0;
            },
            [&queuedRequests](const TileKey& key,
                              TileLoadPriorityGroup group,
                              double priority) {
                queuedRequests.push_back(
                    TileLoadRequest{key, group, priority});
            },
            [](TilesetTile&, double, bool, double) {});

    EXPECT_TRUE(kickedParent);
    ASSERT_EQ(tilePlan.visibleTiles.size(), 1u);
    EXPECT_EQ(tilePlan.visibleTiles.front(), parent.key);
    EXPECT_TRUE(loadQueue.empty());
    ASSERT_EQ(queuedRequests.size(), 1u);
    EXPECT_EQ(queuedRequests.front().key, parent.key);
    EXPECT_EQ(queuedRequests.front().group, TileLoadPriorityGroup::Normal);
    EXPECT_EQ(queuedRequests.front().priority, 5.0);
    EXPECT_EQ(counters.kicked, 1);

    EXPECT_TRUE(result.returnedSingleTileDetails);
    EXPECT_FALSE(result.details.allAreRenderable);
    EXPECT_FALSE(result.details.anyWereRenderedLastFrame);
    EXPECT_EQ(result.details.notYetRenderableCount, 1u);
}

TEST(
    TileSelectionRefinementPolicyTest,
    InitialDecisionRefinesOnlyWhenSseRequiresIt) {
    TileSelectionRefineDecision decision =
        TileSelectionRefinementPolicy::initialRefineDecision(
            false,
            false,
            false);
    EXPECT_TRUE(decision.refine);
    EXPECT_FALSE(decision.meetsSse);

    decision = TileSelectionRefinementPolicy::initialRefineDecision(
        false,
        true,
        false);
    EXPECT_FALSE(decision.refine);
    EXPECT_TRUE(decision.meetsSse);

    decision = TileSelectionRefinementPolicy::initialRefineDecision(
        false,
        false,
        true);
    EXPECT_FALSE(decision.refine);
    EXPECT_FALSE(decision.meetsSse);
}

TEST(
    TileSelectionRefinementPolicyTest,
    UnconditionalRefineOverridesSatisfiedSse) {
    const TileSelectionRefineDecision decision =
        TileSelectionRefinementPolicy::initialRefineDecision(
            true,
            true,
            false);

    EXPECT_TRUE(decision.refine);
    EXPECT_TRUE(decision.meetsSse);
}

TEST(
    TileSelectionRefinementPolicyTest,
    PreviousUnrenderableRefinementContinuesDeeperUrgently) {
    TileSelectionContinueDeeperDecision decision =
        TileSelectionRefinementPolicy::continueDeeperDecision(
            false,
            TileSelectionState::Refined,
            false,
            false);

    EXPECT_TRUE(decision.shouldContinue);
    EXPECT_TRUE(decision.ancestorMeetsSse);
    EXPECT_TRUE(decision.queueUrgent);

    decision = TileSelectionRefinementPolicy::continueDeeperDecision(
        false,
        TileSelectionState::Refined,
        false,
        true);

    EXPECT_TRUE(decision.shouldContinue);
    EXPECT_TRUE(decision.ancestorMeetsSse);
    EXPECT_FALSE(decision.queueUrgent);
}

TEST(
    TileSelectionRefinementPolicyTest,
    ContinueDeeperRequiresPreviousRefinedAndUnrenderableTile) {
    TileSelectionContinueDeeperDecision decision =
        TileSelectionRefinementPolicy::continueDeeperDecision(
            false,
            TileSelectionState::Rendered,
            false,
            false);
    EXPECT_FALSE(decision.shouldContinue);

    decision = TileSelectionRefinementPolicy::continueDeeperDecision(
        false,
        TileSelectionState::Refined,
        true,
        false);
    EXPECT_FALSE(decision.shouldContinue);

    decision = TileSelectionRefinementPolicy::continueDeeperDecision(
        false,
        TileSelectionState::RefinedAndKicked,
        false,
        false);
    EXPECT_TRUE(decision.shouldContinue);
    EXPECT_TRUE(decision.ancestorMeetsSse);
    EXPECT_TRUE(decision.queueUrgent);
}

TEST(
    TileSelectionRefinementPolicyTest,
    OcclusionGateMatchesNativeRefinementCases) {
    EXPECT_TRUE(TileSelectionRefinementPolicy::shouldCheckOcclusion(
        true,
        true,
        false,
        TileSelectionState::Rendered,
        false));
    EXPECT_FALSE(TileSelectionRefinementPolicy::shouldCheckOcclusion(
        true,
        true,
        true,
        TileSelectionState::Rendered,
        false));
    EXPECT_FALSE(TileSelectionRefinementPolicy::shouldCheckOcclusion(
        true,
        true,
        false,
        TileSelectionState::Refined,
        true));
    EXPECT_TRUE(TileSelectionRefinementPolicy::shouldCheckOcclusion(
        true,
        true,
        false,
        TileSelectionState::Refined,
        false));
    EXPECT_TRUE(TileSelectionRefinementPolicy::shouldCheckOcclusion(
        true,
        true,
        false,
        TileSelectionState::RefinedAndKicked,
        true));
    EXPECT_FALSE(TileSelectionRefinementPolicy::shouldCheckOcclusion(
        false,
        true,
        false,
        TileSelectionState::Rendered,
        false));
}

TEST(
    TileSelectionRefinementPolicyTest,
    OcclusionActionStopsOrDelaysRefinement) {
    EXPECT_EQ(
        TileSelectionRefinementPolicy::occlusionAction(
            TileOcclusionState::Occluded,
            false,
            TileSelectionState::Refined),
        TileSelectionOcclusionAction::StopForOccluded);
    EXPECT_EQ(
        TileSelectionRefinementPolicy::occlusionAction(
            TileOcclusionState::OcclusionUnavailable,
            true,
            TileSelectionState::Rendered),
        TileSelectionOcclusionAction::StopForUnavailable);
    EXPECT_EQ(
        TileSelectionRefinementPolicy::occlusionAction(
            TileOcclusionState::OcclusionUnavailable,
            true,
            TileSelectionState::Refined),
        TileSelectionOcclusionAction::None);
    EXPECT_EQ(
        TileSelectionRefinementPolicy::occlusionAction(
            TileOcclusionState::OcclusionUnavailable,
            false,
            TileSelectionState::Rendered),
        TileSelectionOcclusionAction::None);
    EXPECT_EQ(
        TileSelectionRefinementPolicy::occlusionAction(
            TileOcclusionState::NotOccluded,
            true,
            TileSelectionState::Rendered),
        TileSelectionOcclusionAction::None);

    const TileSelectionRefineDecision stopped =
        TileSelectionRefinementPolicy::applyOcclusionAction(
            TileSelectionRefineDecision{true, false},
            TileSelectionOcclusionAction::StopForOccluded);
    EXPECT_FALSE(stopped.refine);
    EXPECT_TRUE(stopped.meetsSse);
}

TEST(
    TileSelectionRefineFlowPolicyTest,
    AppliesOcclusionActionAndCounters) {
    const TileSelectionRefineFlowOptions options{true, true};
    TileSelectionRefineFlowInput input;
    input.meetsScreenSpaceError = false;
    input.renderable = true;
    input.previousSelectionState = TileSelectionState::Rendered;

    TileSelectionRefineFlowResult result =
        TileSelectionRefineFlowPolicy::evaluate(input, options);
    EXPECT_TRUE(result.refine);
    EXPECT_FALSE(result.meetsScreenSpaceError);
    EXPECT_TRUE(result.shouldCheckOcclusion);
    EXPECT_EQ(result.counter, TileSelectionRefineFlowCounter::None);

    input.occlusion = TileOcclusionState::Occluded;
    result = TileSelectionRefineFlowPolicy::evaluate(input, options);
    EXPECT_FALSE(result.refine);
    EXPECT_TRUE(result.meetsScreenSpaceError);
    EXPECT_EQ(result.counter, TileSelectionRefineFlowCounter::Occluded);

    input.occlusion = TileOcclusionState::OcclusionUnavailable;
    result = TileSelectionRefineFlowPolicy::evaluate(input, options);
    EXPECT_FALSE(result.refine);
    EXPECT_TRUE(result.meetsScreenSpaceError);
    EXPECT_EQ(
        result.counter,
        TileSelectionRefineFlowCounter::WaitingForOcclusion);
}

TEST(
    TileSelectionRefineFlowPolicyTest,
    ContinuesDeeperForPreviousUnrenderableRefinement) {
    TileSelectionRefineFlowInput input;
    input.meetsScreenSpaceError = true;
    input.ancestorMeetsSse = false;
    input.renderable = false;
    input.previousSelectionState = TileSelectionState::Refined;

    TileSelectionRefineFlowResult result =
        TileSelectionRefineFlowPolicy::evaluate(
            input,
            TileSelectionRefineFlowOptions{true, true});

    EXPECT_TRUE(result.refine);
    EXPECT_TRUE(result.ancestorMeetsSse);
    EXPECT_TRUE(result.queueUrgentLoad);
    EXPECT_TRUE(result.queuedForLoad);

    input.ancestorMeetsSse = true;
    result = TileSelectionRefineFlowPolicy::evaluate(
        input,
        TileSelectionRefineFlowOptions{true, true});

    EXPECT_TRUE(result.refine);
    EXPECT_TRUE(result.ancestorMeetsSse);
    EXPECT_FALSE(result.queueUrgentLoad);
    EXPECT_FALSE(result.queuedForLoad);
}

TEST(
    TileSelectionRefineFlowPolicyTest,
    SkipsOcclusionForNativeBypassCases) {
    TileSelectionRefineFlowInput input;
    input.unconditionallyRefine = true;
    input.meetsScreenSpaceError = true;
    input.renderable = true;

    TileSelectionRefineFlowResult result =
        TileSelectionRefineFlowPolicy::evaluate(
            input,
            TileSelectionRefineFlowOptions{true, true});
    EXPECT_TRUE(result.refine);
    EXPECT_TRUE(result.meetsScreenSpaceError);
    EXPECT_FALSE(result.shouldCheckOcclusion);

    input = TileSelectionRefineFlowInput{};
    input.meetsScreenSpaceError = false;
    input.renderable = true;
    input.previousSelectionState = TileSelectionState::Refined;
    input.childWasRefinedLastFrame = true;
    result = TileSelectionRefineFlowPolicy::evaluate(
        input,
        TileSelectionRefineFlowOptions{true, true});
    EXPECT_TRUE(result.refine);
    EXPECT_FALSE(result.shouldCheckOcclusion);

    input.childWasRefinedLastFrame = false;
    result = TileSelectionRefineFlowPolicy::evaluate(
        input,
        TileSelectionRefineFlowOptions{false, true});
    EXPECT_TRUE(result.refine);
    EXPECT_FALSE(result.shouldCheckOcclusion);
}
