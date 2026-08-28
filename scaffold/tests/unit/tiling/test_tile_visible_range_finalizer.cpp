#include <gtest/gtest.h>

#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/TilePlan.h"
#include "earth_engine/tiling/TileVisibleRangeFinalizer.h"
#include "earth_engine/tiling/TilesetTile.h"

using namespace earth_engine;

TEST(TileVisibleRangeFinalizerTest, DedupePreservesFirstVisibleOrder) {
    TilePlan plan;
    plan.visibleTiles = {
        TileKey{"test", 2, 1, 0},
        TileKey{"test", 1, 0, 0},
        TileKey{"test", 2, 1, 0},
        TileKey{"test", 3, 2, 0},
        TileKey{"test", 1, 0, 0},
    };

    TileVisibleRangeFinalizer::dedupeVisibleTiles(plan);

    ASSERT_EQ(plan.visibleTiles.size(), 3u);
    EXPECT_EQ(plan.visibleTiles[0], (TileKey{"test", 2, 1, 0}));
    EXPECT_EQ(plan.visibleTiles[1], (TileKey{"test", 1, 0, 0}));
    EXPECT_EQ(plan.visibleTiles[2], (TileKey{"test", 3, 2, 0}));
}

TEST(
    TileVisibleRangeFinalizerTest,
    DedupeKeepsLiveHandlesAlignedWithVisibleTiles) {
    TilesetTile first(
        TileKey{"test", 2, 1, 1},
        Rectangle{});
    TilesetTile second(
        TileKey{"test", 2, 2, 1},
        Rectangle{});
    TilePlan plan;
    plan.visibleTiles = {
        first.key,
        second.key,
        first.key};
    plan.tilesToRenderThisFrame = {
        &first,
        &second,
        &first};

    TileVisibleRangeFinalizer::dedupeVisibleTiles(plan);

    ASSERT_EQ(plan.visibleTiles.size(), 2u);
    ASSERT_EQ(plan.tilesToRenderThisFrame.size(), 2u);
    EXPECT_EQ(plan.visibleTiles[0], first.key);
    EXPECT_EQ(plan.visibleTiles[1], second.key);
    EXPECT_EQ(plan.tilesToRenderThisFrame[0], &first);
    EXPECT_EQ(plan.tilesToRenderThisFrame[1], &second);
}

TEST(TileVisibleRangeFinalizerTest, UpdatesVisibleZoomRangeFromTiles) {
    TilePlan plan;
    plan.zoom = 99;
    plan.minVisibleZoom = 99;
    plan.maxVisibleZoom = 99;
    plan.visibleTiles = {
        TileKey{"test", 4, 8, 0},
        TileKey{"test", 2, 1, 0},
        TileKey{"test", 6, 12, 0},
    };

    TileVisibleRangeFinalizer::updateVisibleZoomRange(plan);

    EXPECT_EQ(plan.minVisibleZoom, 2);
    EXPECT_EQ(plan.maxVisibleZoom, 6);
    EXPECT_EQ(plan.zoom, 6);
}

TEST(TileVisibleRangeFinalizerTest, EmptyVisibleTilesPreserveRangeState) {
    TilePlan plan;
    plan.zoom = 7;
    plan.minVisibleZoom = 3;
    plan.maxVisibleZoom = 5;

    TileVisibleRangeFinalizer::updateVisibleZoomRange(plan);

    EXPECT_EQ(plan.zoom, 7);
    EXPECT_EQ(plan.minVisibleZoom, 3);
    EXPECT_EQ(plan.maxVisibleZoom, 5);
}
