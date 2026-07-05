#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileSelectionSummaryBuilder.h"

using namespace earth_engine;

namespace {

std::unique_ptr<TilesetTile> makeTile(
    TileKey key,
    TileSelectionState state) {
    auto tile = std::make_unique<TilesetTile>(
        key,
        Rectangle{-1.0, -1.0, 1.0, 1.0});
    tile->selectionFrameState.selectionState = state;
    return tile;
}

const TileSelectionRecord* findRecord(
    const TilePlan& plan,
    const TileKey& key) {
    for (const TileSelectionRecord& record : plan.selectionRecords) {
        if (record.key == key) {
            return &record;
        }
    }
    return nullptr;
}

} // namespace

TEST(TileSelectionSummaryBuilderTest, AggregatesVisitedTilesIntoPlan) {
    auto rendered =
        makeTile(TileKey{"test", 0, 0, 0}, TileSelectionState::Rendered);
    auto refined =
        makeTile(TileKey{"test", 1, 0, 0}, TileSelectionState::Refined);
    auto kicked = makeTile(
        TileKey{"test", 1, 1, 0},
        TileSelectionState::RenderedAndKicked);
    auto notVisited =
        makeTile(TileKey{"test", 2, 0, 0}, TileSelectionState::NotVisited);

    rendered->selectionFrameState.screenSpaceError = 5.0;
    rendered->selectionFrameState.cameraInside = true;
    refined->selectionFrameState.inFrustum = true;
    refined->selectionFrameState.ancestorMeetsSse = true;

    // Active-set includes visited tiles + a decaying NotVisited tile + a null
    // slot, exercising the visited filter and null skip.
    const std::vector<TilesetTile*> activeTiles{
        rendered.get(), refined.get(), kicked.get(), notVisited.get(),
        nullptr};

    TilePlan plan;
    plan.visibleTiles.push_back(TileKey{"test", 0, 0, 0});
    plan.visibleTiles.push_back(TileKey{"test", 1, 1, 0});

    const TileSelectionSummaryBuildResult result =
        TileSelectionSummaryBuilder::build(
            plan,
            activeTiles,
            TileSelectionSummaryBuildInput{},
            [](const TilesetTile& tile) {
                return tile.key != TileKey{"test", 1, 1, 0};
            });

    EXPECT_EQ(plan.selectionRecords.size(), 3u);
    ASSERT_NE(findRecord(plan, TileKey{"test", 0, 0, 0}), nullptr);
    EXPECT_DOUBLE_EQ(
        findRecord(plan, TileKey{"test", 0, 0, 0})->screenSpaceError,
        5.0);
    EXPECT_EQ(findRecord(plan, TileKey{"test", 2, 0, 0}), nullptr);

    EXPECT_EQ(plan.selectionRenderedCount, 1);
    EXPECT_EQ(plan.selectionRefinedCount, 1);
    EXPECT_EQ(plan.selectionKickedCount, 1);
    EXPECT_EQ(plan.selectionAncestorMeetsSseCount, 1);
    EXPECT_EQ(plan.cameraInsideNodeCount, 1);
    EXPECT_EQ(plan.inFrustumNodeCount, 1);
    EXPECT_EQ(result.notYetRenderableCount, 1);
}

TEST(TileSelectionSummaryBuilderTest, WritesFrameLevelCounters) {
    auto refined =
        makeTile(TileKey{"test", 0, 0, 0}, TileSelectionState::Refined);
    const std::vector<TilesetTile*> activeTiles{refined.get()};

    TilePlan plan;
    plan.visibleTiles.push_back(TileKey{"test", 0, 0, 0});
    plan.visibleTiles.push_back(TileKey{"test", 1, 0, 0});
    plan.visibleTiles.push_back(TileKey{"test", 1, 1, 0});

    const TileSelectionSummaryBuildResult result =
        TileSelectionSummaryBuilder::build(
            plan,
            activeTiles,
            TileSelectionSummaryBuildInput{
                2,
                5,
                11,
                13,
                17},
            [](const TilesetTile&) { return true; });

    EXPECT_EQ(result.notYetRenderableCount, 0);
    EXPECT_EQ(plan.renderingNodeCount, 3);
    EXPECT_EQ(plan.walkthroughNodeCount, 1);
    EXPECT_EQ(plan.notRenderingNodeCount, 7);
    EXPECT_EQ(plan.selectionOccludedCount, 11);
    EXPECT_EQ(plan.selectionWaitingForOcclusionResultsCount, 13);
    EXPECT_EQ(plan.culledTilesVisitedCount, 17);
    EXPECT_EQ(plan.mercatorTileCount, 3);
}
