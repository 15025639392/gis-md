#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileSelectionStateResetter.h"
#include "earth_engine/tiling/TilesetTileRegistry.h"

using namespace earth_engine;

TEST(TileSelectionStateResetterTest, ResetsSelectionFrameStateForRegistryTiles) {
    TilesetTileRegistry registry;
    auto tile = std::make_unique<TilesetTile>(
        TileKey{"test", 0, 0, 0},
        Rectangle{-1.0, -1.0, 1.0, 1.0});
    TilesetTile* rawTile = tile.get();
    registry.tiles()["root"] = std::move(tile);
    registry.tiles()["null"] = nullptr;

    rawTile->selectionFrameState.selectionState =
        TileSelectionState::RenderedAndKicked;
    rawTile->selectionFrameState.previousSelectionState =
        TileSelectionState::Refined;
    rawTile->selectionFrameState.screenSpaceError = 42.0;
    rawTile->selectionFrameState.inFrustum = true;
    rawTile->selectionFrameState.cameraInside = true;
    rawTile->selectionFrameState.ancestorMeetsSse = true;
    rawTile->selectionFrameState.completeRenderable = true;
    rawTile->selectionFrameState.renderable = true;

    TileSelectionStateResetter::reset(registry, {});

    EXPECT_EQ(
        rawTile->selectionFrameState.previousSelectionState,
        TileSelectionState::RenderedAndKicked);
    EXPECT_EQ(
        rawTile->selectionFrameState.selectionState,
        TileSelectionState::NotVisited);
    EXPECT_DOUBLE_EQ(rawTile->selectionFrameState.screenSpaceError, 0.0);
    EXPECT_FALSE(rawTile->selectionFrameState.inFrustum);
    EXPECT_FALSE(rawTile->selectionFrameState.cameraInside);
    EXPECT_FALSE(rawTile->selectionFrameState.ancestorMeetsSse);
    EXPECT_FALSE(rawTile->selectionFrameState.completeRenderable);
    EXPECT_FALSE(rawTile->selectionFrameState.renderable);
}
