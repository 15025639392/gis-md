#include <gtest/gtest.h>

#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/TileSelectionVisitPreparation.h"
#include "earth_engine/tiling/TilesetTile.h"

using namespace earth_engine;

TEST(
    TileSelectionVisitPreparationTest,
    EarlyExitPlansCulledLoadsAndCounters) {
    TileSelectionVisitPreparationResult preparation;
    preparation.cullResult.culled = true;
    preparation.cullResult.shouldVisit = false;
    preparation.cullResult.reason = TileSelectionCullReason::Frustum;
    preparation.culledLoadPlan.queueLoad = true;
    preparation.culledLoadPlan.group = TileLoadPriorityGroup::Normal;

    TileSelectionVisitEarlyExitPlan plan =
        TileSelectionVisitPreparation::earlyExitPlan(preparation);
    EXPECT_TRUE(plan.shouldExit);
    EXPECT_EQ(plan.reason, TileSelectionVisitEarlyExitReason::Culled);
    EXPECT_EQ(plan.counter, TileSelectionVisitEarlyExitCounter::FrustumCulled);
    EXPECT_TRUE(plan.markTileCulled);
    EXPECT_FALSE(plan.resetScreenSpaceError);
    EXPECT_TRUE(plan.queueCulledLoad);
    EXPECT_EQ(plan.loadGroup, TileLoadPriorityGroup::Normal);

    preparation.cullResult.reason = TileSelectionCullReason::Fog;
    preparation.culledLoadPlan.queueLoad = false;
    plan = TileSelectionVisitPreparation::earlyExitPlan(preparation);
    EXPECT_TRUE(plan.shouldExit);
    EXPECT_EQ(plan.counter, TileSelectionVisitEarlyExitCounter::FogCulled);
    EXPECT_FALSE(plan.queueCulledLoad);
}

TEST(
    TileSelectionVisitPreparationTest,
    EarlyExitPlansViewerRequestVolumeRejection) {
    TileSelectionVisitPreparationResult preparation;
    preparation.viewerRequestVolumeAllowed = false;

    const TileSelectionVisitEarlyExitPlan plan =
        TileSelectionVisitPreparation::earlyExitPlan(preparation);

    EXPECT_TRUE(plan.shouldExit);
    EXPECT_EQ(
        plan.reason,
        TileSelectionVisitEarlyExitReason::ViewerRequestVolume);
    EXPECT_TRUE(plan.markTileCulled);
    EXPECT_TRUE(plan.resetScreenSpaceError);
    EXPECT_EQ(plan.counter, TileSelectionVisitEarlyExitCounter::None);
    EXPECT_FALSE(plan.queueCulledLoad);
}

TEST(
    TileSelectionVisitPreparationTest,
    EarlyExitAllowsVisitableTileToContinue) {
    const TileSelectionVisitPreparationResult preparation;

    const TileSelectionVisitEarlyExitPlan plan =
        TileSelectionVisitPreparation::earlyExitPlan(preparation);

    EXPECT_FALSE(plan.shouldExit);
    EXPECT_EQ(plan.reason, TileSelectionVisitEarlyExitReason::None);
}

TEST(TileSelectionVisitPreparationTest, OutcomePlanMapsEarlyExits) {
    TileSelectionVisitPreparationResult preparation;
    preparation.cullResult.culled = true;
    preparation.cullResult.shouldVisit = false;
    preparation.cullResult.reason = TileSelectionCullReason::Frustum;
    preparation.culledLoadPlan.queueLoad = true;
    preparation.culledLoadPlan.group = TileLoadPriorityGroup::Normal;

    TileSelectionVisitOutcomePlan outcome =
        TileSelectionVisitPreparation::outcomePlan(preparation);
    EXPECT_TRUE(outcome.shouldExit);
    EXPECT_EQ(outcome.exitReason, TileSelectionVisitEarlyExitReason::Culled);
    EXPECT_EQ(
        outcome.counter,
        TileSelectionVisitEarlyExitCounter::FrustumCulled);
    EXPECT_TRUE(outcome.markTileCulled);
    EXPECT_FALSE(outcome.resetScreenSpaceError);
    EXPECT_TRUE(outcome.queueLoad);
    EXPECT_EQ(outcome.loadGroup, TileLoadPriorityGroup::Normal);
    EXPECT_TRUE(outcome.returnCulledTraversalDetails);
    EXPECT_FALSE(outcome.countCulledVisited);

    preparation.cullResult.reason = TileSelectionCullReason::Fog;
    preparation.culledLoadPlan.queueLoad = false;
    outcome = TileSelectionVisitPreparation::outcomePlan(preparation);
    EXPECT_TRUE(outcome.shouldExit);
    EXPECT_EQ(outcome.counter, TileSelectionVisitEarlyExitCounter::FogCulled);
    EXPECT_FALSE(outcome.queueLoad);
    EXPECT_TRUE(outcome.returnCulledTraversalDetails);
}

TEST(
    TileSelectionVisitPreparationTest,
    OutcomePlanMapsViewerRequestVolumeRejection) {
    TileSelectionVisitPreparationResult preparation;
    preparation.viewerRequestVolumeAllowed = false;

    const TileSelectionVisitOutcomePlan outcome =
        TileSelectionVisitPreparation::outcomePlan(preparation);

    EXPECT_TRUE(outcome.shouldExit);
    EXPECT_EQ(
        outcome.exitReason,
        TileSelectionVisitEarlyExitReason::ViewerRequestVolume);
    EXPECT_TRUE(outcome.markTileCulled);
    EXPECT_TRUE(outcome.resetScreenSpaceError);
    EXPECT_FALSE(outcome.queueLoad);
    EXPECT_FALSE(outcome.returnCulledTraversalDetails);
}

TEST(TileSelectionVisitPreparationTest, OutcomePlanCountsVisitableCulledTiles) {
    TileSelectionVisitPreparationResult preparation;
    preparation.cullResult.culled = true;
    preparation.cullResult.shouldVisit = true;

    TileSelectionVisitOutcomePlan outcome =
        TileSelectionVisitPreparation::outcomePlan(preparation);
    EXPECT_FALSE(outcome.shouldExit);
    EXPECT_TRUE(outcome.countCulledVisited);
    EXPECT_FALSE(outcome.markTileCulled);

    preparation.cullResult.culled = false;
    outcome = TileSelectionVisitPreparation::outcomePlan(preparation);
    EXPECT_FALSE(outcome.shouldExit);
    EXPECT_FALSE(outcome.countCulledVisited);
}

TEST(
    TileSelectionVisitPreparationTest,
    CulledUnconditionallyRefinedRootStillVisits) {
    TilesetTile root(
        TileKey{"test", 0, 0, 0},
        Rectangle{-0.25, -0.25, 0.25, 0.25});
    root.unconditionallyRefine = true;

    std::vector<double> scratchDistances;
    const TileSelectionVisitPreparationResult result =
        TileSelectionVisitPreparation::prepare(
            root,
            {},
            {0.0},
            TileSelectionVisibilityContext{false, 0.0, 0.0},
            TileSelectionVisitPreparationOptions{
                true,
                true,
                false,
                false,
                true,
                16.0,
                64.0},
            scratchDistances);

    EXPECT_TRUE(result.cullResult.culled);
    EXPECT_TRUE(result.cullResult.shouldVisit);
}

TEST(
    TileSelectionVisitPreparationTest,
    ForbidHolesReplaceChildKeepsCulledUnconditionalVisit) {
    TilesetTile parent(
        TileKey{"test", 0, 0, 0},
        Rectangle{-0.25, -0.25, 0.25, 0.25});
    TilesetTile child(
        TileKey{"test", 1, 0, 0},
        Rectangle{-0.25, -0.25, 0.0, 0.0},
        &parent);
    child.unconditionallyRefine = true;
    child.refine = TileRefine::Replace;

    std::vector<double> scratchDistances;
    const TileSelectionVisitPreparationResult result =
        TileSelectionVisitPreparation::prepare(
            child,
            {},
            {0.0},
            TileSelectionVisibilityContext{false, 0.0, 0.0},
            TileSelectionVisitPreparationOptions{
                true,
                true,
                false,
                true,
                true,
                16.0,
                64.0},
            scratchDistances);

    EXPECT_TRUE(result.cullResult.culled);
    EXPECT_TRUE(result.cullResult.shouldVisit);
}

TEST(
    TileSelectionVisitPreparationTest,
    AddChildStillExitsWhenCulledAndUnconditionallyRefined) {
    TilesetTile parent(
        TileKey{"test", 0, 0, 0},
        Rectangle{-0.25, -0.25, 0.25, 0.25});
    TilesetTile child(
        TileKey{"test", 1, 0, 0},
        Rectangle{-0.25, -0.25, 0.0, 0.0},
        &parent);
    child.unconditionallyRefine = true;
    child.refine = TileRefine::Add;

    std::vector<double> scratchDistances;
    const TileSelectionVisitPreparationResult result =
        TileSelectionVisitPreparation::prepare(
            child,
            {},
            {0.0},
            TileSelectionVisibilityContext{false, 0.0, 0.0},
            TileSelectionVisitPreparationOptions{
                true,
                true,
                false,
                true,
                true,
                16.0,
                64.0},
            scratchDistances);

    EXPECT_TRUE(result.cullResult.culled);
    EXPECT_FALSE(result.cullResult.shouldVisit);
}
