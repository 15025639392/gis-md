#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionSummaryPolicy.h"

using namespace earth_engine;

TEST(TileSelectionSummaryPolicyTest, PlansVisitedTileRecordSnapshot) {
    TileSelectionFrameState state;
    state.selectionState = TileSelectionState::RenderedAndKicked;
    state.previousSelectionState = TileSelectionState::Refined;
    state.screenSpaceError = 12.5;
    state.cameraInside = true;
    state.inFrustum = true;
    state.ancestorMeetsSse = true;

    const TileSelectionSummaryTilePlan plan =
        TileSelectionSummaryPolicy::planTile(
            TileSelectionSummaryTileInput{
                TileKey{"test", 3, 4, 5},
                state,
                false});

    EXPECT_TRUE(plan.visited);
    EXPECT_EQ(plan.record.key, (TileKey{"test", 3, 4, 5}));
    EXPECT_EQ(plan.record.state, TileSelectionState::RenderedAndKicked);
    EXPECT_EQ(plan.record.previousState, TileSelectionState::Refined);
    EXPECT_DOUBLE_EQ(plan.record.screenSpaceError, 12.5);
    EXPECT_TRUE(plan.record.cameraInside);
    EXPECT_TRUE(plan.record.inFrustum);
    EXPECT_TRUE(plan.record.ancestorMeetsSse);
}

TEST(TileSelectionSummaryPolicyTest, CountsDiagnosticsFromVisitedTile) {
    TileSelectionFrameState state;
    state.selectionState = TileSelectionState::RenderedAndKicked;
    state.cameraInside = true;
    state.inFrustum = true;
    state.ancestorMeetsSse = true;

    TileSelectionSummaryTilePlan plan =
        TileSelectionSummaryPolicy::planTile(
            TileSelectionSummaryTileInput{
                TileKey{"test", 0, 0, 0},
                state,
                false});

    EXPECT_EQ(plan.selectionKickedCount, 1);
    EXPECT_EQ(plan.selectionAncestorMeetsSseCount, 1);
    EXPECT_EQ(plan.cameraInsideNodeCount, 1);
    EXPECT_EQ(plan.inFrustumNodeCount, 1);
    EXPECT_EQ(plan.notYetRenderableCount, 1);
    EXPECT_EQ(plan.selectionRenderedCount, 0);
    EXPECT_EQ(plan.selectionRefinedCount, 0);

    state.selectionState = TileSelectionState::Rendered;
    plan = TileSelectionSummaryPolicy::planTile(
        TileSelectionSummaryTileInput{
            TileKey{"test", 0, 0, 0},
            state,
            true});

    EXPECT_EQ(plan.selectionRenderedCount, 1);
    EXPECT_EQ(plan.selectionRefinedCount, 0);
    EXPECT_EQ(plan.selectionKickedCount, 0);
    EXPECT_EQ(plan.notYetRenderableCount, 0);

    state.selectionState = TileSelectionState::Refined;
    plan = TileSelectionSummaryPolicy::planTile(
        TileSelectionSummaryTileInput{
            TileKey{"test", 0, 0, 0},
            state,
            true});

    EXPECT_EQ(plan.selectionRenderedCount, 0);
    EXPECT_EQ(plan.selectionRefinedCount, 1);
}

TEST(TileSelectionSummaryPolicyTest, SkipsNotVisitedTiles) {
    TileSelectionFrameState state;
    state.selectionState = TileSelectionState::NotVisited;
    state.previousSelectionState = TileSelectionState::Rendered;
    state.screenSpaceError = 1.0;
    state.cameraInside = true;
    state.inFrustum = true;
    state.ancestorMeetsSse = true;

    const TileSelectionSummaryTilePlan plan =
        TileSelectionSummaryPolicy::planTile(
            TileSelectionSummaryTileInput{
                TileKey{"test", 0, 0, 0},
                state,
                false});

    EXPECT_FALSE(plan.visited);
    EXPECT_EQ(plan.selectionKickedCount, 0);
    EXPECT_EQ(plan.notYetRenderableCount, 0);
}

TEST(TileSelectionSummaryPolicyTest, DerivesFrameCounters) {
    const TileSelectionSummaryFramePlan plan =
        TileSelectionSummaryPolicy::planFrame(
            TileSelectionSummaryFrameInput{
                7,
                3,
                2,
                5,
                11,
                13,
                17});

    EXPECT_EQ(plan.renderingNodeCount, 7);
    EXPECT_EQ(plan.walkthroughNodeCount, 3);
    EXPECT_EQ(plan.notRenderingNodeCount, 7);
    EXPECT_EQ(plan.selectionOccludedCount, 11);
    EXPECT_EQ(plan.selectionWaitingForOcclusionResultsCount, 13);
    EXPECT_EQ(plan.culledTilesVisitedCount, 17);
    EXPECT_EQ(plan.mercatorTileCount, 7);
}
