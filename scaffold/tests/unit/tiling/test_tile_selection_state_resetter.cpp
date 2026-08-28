#include <gtest/gtest.h>

#include "earth_engine/tiling/DirectRasterMapping.h"
#include "earth_engine/tiling/TileSelectionStateResetter.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "../../helpers/RasterOverlayTestFrame.h"

using namespace earth_engine;

TEST(TileSelectionStateResetterTest, ResetsSelectionFrameStateForTile) {
    TilesetTile tile(
        TileKey{"test", 0, 0, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});

    tile.selectionFrameState.selectionState =
        TileSelectionState::RenderedAndKicked;
    tile.selectionFrameState.previousSelectionState =
        TileSelectionState::Refined;
    tile.selectionFrameState.screenSpaceError = 42.0;
    tile.selectionFrameState.inFrustum = true;
    tile.selectionFrameState.cameraInside = true;
    tile.selectionFrameState.ancestorMeetsSse = true;
    tile.selectionFrameState.completeRenderable = true;
    tile.selectionFrameState.renderable = true;

    TileSelectionStateResetter::resetOne(
        tile,
        earth_engine::testing::emptyRasterOverlayFrame());

    EXPECT_EQ(
        tile.selectionFrameState.previousSelectionState,
        TileSelectionState::RenderedAndKicked);
    EXPECT_EQ(
        tile.selectionFrameState.selectionState,
        TileSelectionState::NotVisited);
    EXPECT_DOUBLE_EQ(tile.selectionFrameState.screenSpaceError, 0.0);
    EXPECT_FALSE(tile.selectionFrameState.inFrustum);
    EXPECT_FALSE(tile.selectionFrameState.cameraInside);
    EXPECT_FALSE(tile.selectionFrameState.ancestorMeetsSse);
    EXPECT_FALSE(tile.selectionFrameState.completeRenderable);
    EXPECT_FALSE(tile.selectionFrameState.renderable);
}
