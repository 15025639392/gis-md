#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileLoadQueue.h"
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/tiling/TileSelectionPlanAppender.h"
#include "earth_engine/tiling/TilesetTile.h"

using namespace earth_engine;

TEST(TileSelectionPlanAppenderTest, AddsVisibleTileAndQueuesNormalLoad) {
    TilePlan tilePlan;
    TileLoadQueue loadQueue;
    TilesetTile tile(
        TileKey{"test", 0, 0, 0},
        Rectangle{});
    tile.selectionFrameState.selectionState = TileSelectionState::NotVisited;
    tile.selectionFrameState.screenSpaceError = 0.0;

    TileSelectionPlanAppender::addTileToCurrentPlan(
        tilePlan,
        loadQueue,
        tile,
        12.5,
        true,
        3.0);

    ASSERT_EQ(tilePlan.visibleTiles.size(), 1u);
    EXPECT_EQ(tilePlan.visibleTiles.front(), tile.key);
    ASSERT_EQ(tilePlan.tilesToRenderThisFrame.size(), 1u);
    EXPECT_EQ(tilePlan.tilesToRenderThisFrame.front(), &tile);
    EXPECT_EQ(tile.selectionFrameState.selectionState,
              TileSelectionState::Rendered);
    EXPECT_EQ(tile.selectionFrameState.screenSpaceError, 12.5);
    ASSERT_EQ(loadQueue.requests().size(), 1u);
    EXPECT_EQ(loadQueue.requests().front().key, tile.key);
    EXPECT_EQ(
        loadQueue.requests().front().group,
        TileLoadPriorityGroup::Normal);
    EXPECT_EQ(loadQueue.requests().front().priority, 3.0);
}

TEST(TileSelectionPlanAppenderTest, SkipsNormalLoadWhenAlreadyQueued) {
    TilePlan tilePlan;
    TileLoadQueue loadQueue;
    TilesetTile tile(
        TileKey{"test", 0, 1, 0},
        Rectangle{});

    TileSelectionPlanAppender::addTileToCurrentPlan(
        tilePlan,
        loadQueue,
        tile,
        4.0,
        false,
        9.0);

    ASSERT_EQ(tilePlan.visibleTiles.size(), 1u);
    ASSERT_EQ(tilePlan.tilesToRenderThisFrame.size(), 1u);
    EXPECT_EQ(tilePlan.tilesToRenderThisFrame.front(), &tile);
    EXPECT_EQ(tile.selectionFrameState.selectionState,
              TileSelectionState::Rendered);
    EXPECT_EQ(tile.selectionFrameState.screenSpaceError, 4.0);
    EXPECT_TRUE(loadQueue.empty());
}
