#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionTraversalCounterPolicy.h"

using namespace earth_engine;

TEST(TileSelectionTraversalCounterPolicyTest, CountsAcceptedVisitsOnly) {
    const TileSelectionTraversalCounterPlan counters =
        TileSelectionTraversalCounterPolicy::planVisitAccepted();

    EXPECT_EQ(counters.visited, 1);
    EXPECT_EQ(counters.frustumCulled, 0);
    EXPECT_EQ(counters.fogCulled, 0);
    EXPECT_EQ(counters.culledVisited, 0);
}

TEST(TileSelectionTraversalCounterPolicyTest, CountsEarlyExitOutcomes) {
    TileSelectionVisitOutcomePlan outcome;
    outcome.shouldExit = true;
    outcome.counter = TileSelectionVisitEarlyExitCounter::FrustumCulled;

    TileSelectionTraversalCounterPlan counters =
        TileSelectionTraversalCounterPolicy::planOutcome(outcome);
    EXPECT_EQ(counters.frustumCulled, 1);
    EXPECT_EQ(counters.fogCulled, 0);
    EXPECT_EQ(counters.culledVisited, 0);

    outcome.counter = TileSelectionVisitEarlyExitCounter::FogCulled;
    counters = TileSelectionTraversalCounterPolicy::planOutcome(outcome);
    EXPECT_EQ(counters.frustumCulled, 0);
    EXPECT_EQ(counters.fogCulled, 1);
    EXPECT_EQ(counters.culledVisited, 0);
}

TEST(TileSelectionTraversalCounterPolicyTest, CountsVisitableCulledTiles) {
    TileSelectionVisitOutcomePlan outcome;
    outcome.countCulledVisited = true;

    const TileSelectionTraversalCounterPlan counters =
        TileSelectionTraversalCounterPolicy::planOutcome(outcome);

    EXPECT_EQ(counters.frustumCulled, 0);
    EXPECT_EQ(counters.fogCulled, 0);
    EXPECT_EQ(counters.culledVisited, 1);
}

TEST(TileSelectionTraversalCounterPolicyTest, CountsRefineFlowOcclusion) {
    TileSelectionRefineFlowResult refineFlow;
    refineFlow.counter = TileSelectionRefineFlowCounter::Occluded;

    TileSelectionTraversalCounterPlan counters =
        TileSelectionTraversalCounterPolicy::planRefineFlow(refineFlow);
    EXPECT_EQ(counters.occluded, 1);
    EXPECT_EQ(counters.waitingForOcclusion, 0);

    refineFlow.counter = TileSelectionRefineFlowCounter::WaitingForOcclusion;
    counters =
        TileSelectionTraversalCounterPolicy::planRefineFlow(refineFlow);
    EXPECT_EQ(counters.occluded, 0);
    EXPECT_EQ(counters.waitingForOcclusion, 1);

    refineFlow.counter = TileSelectionRefineFlowCounter::None;
    counters =
        TileSelectionTraversalCounterPolicy::planRefineFlow(refineFlow);
    EXPECT_EQ(counters.occluded, 0);
    EXPECT_EQ(counters.waitingForOcclusion, 0);
}

TEST(TileSelectionTraversalCounterPolicyTest, CountsPostTraversalKicks) {
    TileSelectionPostTraversalCommitPlan commitPlan;
    commitPlan.trimRenderedDescendants = true;

    TileSelectionTraversalCounterPlan counters =
        TileSelectionTraversalCounterPolicy::planPostTraversalCommit(
            commitPlan);
    EXPECT_EQ(counters.kicked, 1);

    commitPlan.trimRenderedDescendants = false;
    counters = TileSelectionTraversalCounterPolicy::planPostTraversalCommit(
        commitPlan);
    EXPECT_EQ(counters.kicked, 0);
}
