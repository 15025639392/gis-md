#include <gtest/gtest.h>

#include "earth_engine/tiling/TileLoadRequestPlanner.h"

using namespace earth_engine;

TEST(TileLoadRequestPlannerTest, ClassifiesBasicRequestKinds) {
    TileLoadRequestSnapshot snapshot;
    snapshot.terrainProviderSupportsTile = true;

    EXPECT_EQ(
        TileLoadRequestKind::Terrain,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.contentProviderSupportsTile = true;

    EXPECT_EQ(
        TileLoadRequestKind::Content,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.hasTile = true;
    snapshot.hasRenderContent = true;

    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, ClassifiesUnloadedTilesAsRequestable) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.loadState = TileLoadState::Unloaded;
    snapshot.terrainProviderSupportsTile = true;

    EXPECT_EQ(
        TileLoadRequestKind::Terrain,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.contentProviderSupportsTile = true;

    EXPECT_EQ(
        TileLoadRequestKind::Content,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, ClassifiesUpsampledAndCachedTerrain) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.upsampledFromParent = true;
    snapshot.loadState = TileLoadState::FailedTemporarily;

    EXPECT_EQ(
        TileLoadRequestKind::UpsampledTerrain,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot = TileLoadRequestSnapshot{};
    snapshot.terrainProviderSupportsTile = true;
    snapshot.terrainAlreadyCached = true;

    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));
}
