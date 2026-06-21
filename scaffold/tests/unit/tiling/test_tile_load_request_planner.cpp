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

TEST(TileLoadRequestPlannerTest, UpsampledTerrainTakesLocalPathBeforeProviderChecks) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.upsampledFromParent = true;
    snapshot.loadState = TileLoadState::Unloaded;
    snapshot.terrainProviderSupportsTile = false;
    snapshot.terrainAlreadyCached = true;
    snapshot.hasRenderContent = true;

    EXPECT_EQ(
        TileLoadRequestKind::UpsampledTerrain,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, SkipsNonRetryableLoadStates) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.terrainProviderSupportsTile = true;

    snapshot.loadState = TileLoadState::ContentLoading;
    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.loadState = TileLoadState::Unloading;
    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.loadState = TileLoadState::Failed;
    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, ClassifiesTemporarilyFailedTilesAsRetryable) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.loadState = TileLoadState::FailedTemporarily;
    snapshot.terrainProviderSupportsTile = true;

    EXPECT_EQ(
        TileLoadRequestKind::Terrain,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.contentProviderSupportsTile = true;

    EXPECT_EQ(
        TileLoadRequestKind::Content,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.loadState = TileLoadState::Failed;

    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));
}
