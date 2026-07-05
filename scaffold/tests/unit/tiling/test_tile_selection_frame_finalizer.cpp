#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileSelectionFrameFinalizer.h"

using namespace earth_engine;

TEST(
    TileSelectionFrameFinalizerTest,
    FinalizesVisibleRangeTransitionsAndSummary) {
    TilePlan plan;
    plan.visibleTiles = {
        TileKey{"test", 4, 8, 0},
        TileKey{"test", 2, 1, 0},
        TileKey{"test", 4, 8, 0},
    };

    auto tile = std::make_unique<TilesetTile>(
        TileKey{"test", 4, 8, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});
    tile->selectionFrameState.selectionState = TileSelectionState::Rendered;
    tile->selectionFrameState.screenSpaceError = 12.0;
    tile->selectionFrameState.cameraInside = true;
    const std::vector<TilesetTile*> activeTiles{tile.get()};

    TileSelectionCounters counters;
    counters.culled = 2;
    counters.fogCulled = 3;
    counters.occluded = 5;
    counters.waitingForOcclusionResults = 7;
    counters.culledVisited = 11;

    bool updateTransitionsCalled = false;
    double updateDeltaSeconds = 0.0;
    bool refreshRenderEntriesCalled = false;

    const TileSelectionFrameFinalizeTimings timings =
        TileSelectionFrameFinalizer::finalize(
            plan,
            activeTiles,
            counters,
            0.25,
            [&](double deltaSeconds) {
                updateTransitionsCalled = true;
                updateDeltaSeconds = deltaSeconds;
            },
            [&]() {
                refreshRenderEntriesCalled = true;
                plan.renderEntries.push_back(
                    TileRenderEntry{
                        TileKey{"test", 4, 8, 0},
                        TileKey{"test", 4, 8, 0}});
            },
            [](const TilesetTile&) { return false; });

    EXPECT_TRUE(updateTransitionsCalled);
    EXPECT_DOUBLE_EQ(updateDeltaSeconds, 0.25);
    EXPECT_TRUE(refreshRenderEntriesCalled);
    EXPECT_GE(timings.dedupeMs, 0.0);
    EXPECT_GE(timings.transitionMs, 0.0);
    EXPECT_GE(timings.summaryMs, 0.0);

    ASSERT_EQ(plan.visibleTiles.size(), 2u);
    EXPECT_EQ(plan.visibleTiles[0], (TileKey{"test", 4, 8, 0}));
    EXPECT_EQ(plan.visibleTiles[1], (TileKey{"test", 2, 1, 0}));
    EXPECT_EQ(plan.minVisibleZoom, 2);
    EXPECT_EQ(plan.maxVisibleZoom, 4);
    EXPECT_EQ(plan.zoom, 4);

    ASSERT_EQ(plan.renderEntries.size(), 1u);
    ASSERT_EQ(plan.selectionRecords.size(), 1u);
    EXPECT_EQ(plan.selectionRecords.front().key, (TileKey{"test", 4, 8, 0}));
    EXPECT_EQ(plan.selectionRenderedCount, 1);
    EXPECT_EQ(plan.cameraInsideNodeCount, 1);
    EXPECT_EQ(plan.renderingNodeCount, 2);
    EXPECT_EQ(plan.notRenderingNodeCount, 5);
    EXPECT_EQ(plan.selectionOccludedCount, 5);
    EXPECT_EQ(plan.selectionWaitingForOcclusionResultsCount, 7);
    EXPECT_EQ(plan.culledTilesVisitedCount, 11);
    EXPECT_EQ(plan.mercatorTileCount, 2);
    EXPECT_EQ(counters.notYetRenderable, 1);
}
