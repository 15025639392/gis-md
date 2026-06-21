#include <gtest/gtest.h>

#include "earth_engine/tiling/TileEmptyContentRegistry.h"
#include "earth_engine/tiling/TilePendingLoadCommitCoordinator.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"

using namespace earth_engine;

TEST(TilePendingLoadCommitCoordinatorTest,
     MissingTileTerminalResultsHaveNoSideEffects) {
    const TileKey terrainKey{"test", 0, 0, 0};
    const TileKey contentKey{"test", 0, 1, 0};
    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert("missing-terrain");
    emptyContentRegistry.insert("missing-content");
    PendingTerrainTerminalResult terrainResult{
        terrainKey,
        "missing-terrain",
        TileLoadPriorityGroup::Normal,
        0.0,
        TerrainTileLoadStatus::RetryLater};
    PendingContentTerminalResult contentResult{
        contentKey,
        "missing-content",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileContentLoadStatus::RetryLater};
    bool childrenEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitTerrainTerminalResult(
        terrainResult,
        emptyContentRegistry,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [&resourcesDirty]() { resourcesDirty = true; });
    TilePendingLoadCommitCoordinator::commitContentTerminalResult(
        contentResult,
        emptyContentRegistry,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [&childrenEnsured](TilesetTile&) { childrenEnsured = true; },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_FALSE(childrenEnsured);
    EXPECT_FALSE(resourcesDirty);
    EXPECT_TRUE(emptyContentRegistry.contains("missing-terrain"));
    EXPECT_TRUE(emptyContentRegistry.contains("missing-content"));
}
