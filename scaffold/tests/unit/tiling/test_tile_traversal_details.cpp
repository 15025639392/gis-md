#include <gtest/gtest.h>

#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/RasterOverlayRuntime.h"
#include "earth_engine/tiling/TileSelectionTraversalDetailsBuilder.h"
#include "earth_engine/tiling/TileTraversalDetails.h"
#include "earth_engine/tiling/TilesetTile.h"

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

TEST(TileTraversalDetailsPolicyTest, AggregatesChildren) {
    TileTraversalDetails aggregate;
    TileTraversalDetailsPolicy::mergeChild(
        aggregate,
        TileTraversalDetailsPolicy::forSingleTile(true, true));
    TileTraversalDetailsPolicy::mergeChild(
        aggregate,
        TileTraversalDetailsPolicy::forSingleTile(false, false));

    EXPECT_FALSE(aggregate.allAreRenderable);
    EXPECT_TRUE(aggregate.anyWereRenderedLastFrame);
    EXPECT_EQ(aggregate.notYetRenderableCount, 1u);
}

TEST(
    TileSelectionTraversalDetailsBuilderTest,
    ReplaceTileInheritsRenderedDescendantHistory) {
    TilesetTile parent(TileKey{"test", 0, 0, 0}, Rectangle{});
    TilesetTile child(TileKey{"test", 1, 0, 0}, Rectangle{}, &parent);
    parent.children.push_back(&child);
    parent.refine = TileRefine::Replace;
    parent.content.loadState = TileLoadState::Done;
    parent.content.contentKind = TileContentKind::Empty;
    parent.selectionFrameState.previousSelectionState =
        TileSelectionState::Refined;
    child.selectionFrameState.previousSelectionState =
        TileSelectionState::Rendered;

    const TileTraversalDetails details =
        TileSelectionTraversalDetailsBuilder::forSingleTile(
            parent, RasterOverlayFrameContext{});

    EXPECT_TRUE(details.allAreRenderable);
    EXPECT_TRUE(details.anyWereRenderedLastFrame);
    EXPECT_EQ(details.notYetRenderableCount, 0u);
}

TEST(
    TileSelectionTraversalDetailsBuilderTest,
    ForbidHolesCulledReplaceTileBlocksTraversalWhenMissing) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.refine = TileRefine::Replace;
    tile.content.loadState = TileLoadState::Unloaded;
    tile.content.contentKind = TileContentKind::Unknown;
    tile.selectionFrameState.previousSelectionState =
        TileSelectionState::Rendered;

    const TileTraversalDetails details =
        TileSelectionTraversalDetailsBuilder::forCulledTile(
            tile,
            true,
            RasterOverlayFrameContext{});

    EXPECT_FALSE(details.allAreRenderable);
    EXPECT_FALSE(details.anyWereRenderedLastFrame);
    EXPECT_EQ(details.notYetRenderableCount, 1u);
}
