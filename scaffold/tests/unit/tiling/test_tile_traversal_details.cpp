#include <gtest/gtest.h>

#include "earth_engine/tiling/TileTraversalDetails.h"

using namespace earth_engine;

TEST(TileTraversalDetailsPolicyTest, SummarizesSingleAndCulledTiles) {
    EXPECT_FALSE(
        TileTraversalDetailsPolicy::wasRenderedLastFrameForTraversalDetails(
            TileSelectionState::RenderedAndKicked,
            TileRefine::Replace,
            false));
    EXPECT_TRUE(
        TileTraversalDetailsPolicy::wasRenderedLastFrameForTraversalDetails(
            TileSelectionState::Refined,
            TileRefine::Add,
            false));
    EXPECT_TRUE(
        TileTraversalDetailsPolicy::wasRenderedLastFrameForTraversalDetails(
            TileSelectionState::Refined,
            TileRefine::Replace,
            true));
    EXPECT_FALSE(
        TileTraversalDetailsPolicy::wasRenderedLastFrameForTraversalDetails(
            TileSelectionState::Refined,
            TileRefine::Replace,
            false));
    EXPECT_FALSE(
        TileTraversalDetailsPolicy::wasRenderedLastFrameForTraversalDetails(
            TileSelectionState::RefinedAndKicked,
            TileRefine::Add,
            false));
    EXPECT_FALSE(
        TileTraversalDetailsPolicy::wasRenderedLastFrameForTraversalDetails(
            TileSelectionState::RefinedAndKicked,
            TileRefine::Replace,
            true));

    const TileTraversalDetails rendered =
        TileTraversalDetailsPolicy::forSingleTile(true, true);
    EXPECT_TRUE(rendered.allAreRenderable);
    EXPECT_TRUE(rendered.anyWereRenderedLastFrame);
    EXPECT_EQ(rendered.notYetRenderableCount, 0u);

    const TileTraversalDetails missing =
        TileTraversalDetailsPolicy::forSingleTile(false, true);
    EXPECT_FALSE(missing.allAreRenderable);
    EXPECT_FALSE(missing.anyWereRenderedLastFrame);
    EXPECT_EQ(missing.notYetRenderableCount, 1u);

    const TileTraversalDetails ignoredCulled =
        TileTraversalDetailsPolicy::forCulledTile(
            false,
            TileRefine::Replace,
            false,
            true);
    EXPECT_TRUE(ignoredCulled.allAreRenderable);
    EXPECT_FALSE(ignoredCulled.anyWereRenderedLastFrame);
    EXPECT_EQ(ignoredCulled.notYetRenderableCount, 0u);

    const TileTraversalDetails forbidHolesCulled =
        TileTraversalDetailsPolicy::forCulledTile(
            true,
            TileRefine::Replace,
            false,
            true);
    EXPECT_FALSE(forbidHolesCulled.allAreRenderable);
    EXPECT_FALSE(forbidHolesCulled.anyWereRenderedLastFrame);
    EXPECT_EQ(forbidHolesCulled.notYetRenderableCount, 1u);
}
