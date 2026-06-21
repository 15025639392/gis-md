#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileSelectionHistory.h"
#include "earth_engine/tiling/TilesetTile.h"

using namespace earth_engine;

TEST(TileSelectionHistoryTest, ReadsPreviousSelectionTreeLikeNativeGetResult) {
    TilesetTile root(
        TileKey{"test", 0, 0, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});
    TilesetTile renderedChild(
        TileKey{"test", 1, 0, 0},
        Rectangle{-1.0, -1.0, 0.0, 0.0},
        &root);
    TilesetTile refinedChild(
        TileKey{"test", 1, 1, 0},
        Rectangle{0.0, -1.0, 1.0, 0.0},
        &root);
    TilesetTile grandchild(
        TileKey{"test", 2, 2, 0},
        Rectangle{0.0, -1.0, 0.5, -0.5},
        &refinedChild);

    root.children.push_back(nullptr);
    root.children.push_back(&renderedChild);
    root.children.push_back(&refinedChild);
    refinedChild.children.push_back(&grandchild);

    renderedChild.selectionFrameState.previousSelectionState =
        TileSelectionState::Rendered;
    refinedChild.selectionFrameState.previousSelectionState =
        TileSelectionState::RefinedAndKicked;
    grandchild.selectionFrameState.previousSelectionState =
        TileSelectionState::NotVisited;

    EXPECT_TRUE(TileSelectionHistory::wasRenderedLastFrame(renderedChild));
    renderedChild.selectionFrameState.previousSelectionState =
        TileSelectionState::RenderedAndKicked;
    EXPECT_FALSE(TileSelectionHistory::wasRenderedLastFrame(renderedChild));
    EXPECT_FALSE(TileSelectionHistory::wasRenderedLastFrame(refinedChild));
    EXPECT_FALSE(TileSelectionHistory::childWasRefinedLastFrame(root));
    EXPECT_FALSE(TileSelectionHistory::anyDescendantWasRenderedLastFrame(root));

    renderedChild.selectionFrameState.previousSelectionState =
        TileSelectionState::NotVisited;
    refinedChild.selectionFrameState.previousSelectionState =
        TileSelectionState::Refined;
    EXPECT_TRUE(TileSelectionHistory::childWasRefinedLastFrame(root));
    grandchild.selectionFrameState.previousSelectionState =
        TileSelectionState::Rendered;
    EXPECT_TRUE(TileSelectionHistory::anyDescendantWasRenderedLastFrame(root));

    grandchild.selectionFrameState.previousSelectionState =
        TileSelectionState::NotVisited;
    EXPECT_FALSE(TileSelectionHistory::anyDescendantWasRenderedLastFrame(root));
}

