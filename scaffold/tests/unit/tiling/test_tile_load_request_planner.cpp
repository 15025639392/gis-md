#include <gtest/gtest.h>

#include "earth_engine/tiling/TileLoadRequestPlanner.h"

using namespace earth_engine;

TEST(TileLoadRequestPlannerTest, SkipsProviderlessTerrainRequests) {
    TileLoadRequestSnapshot snapshot;

    EXPECT_EQ(
        TileLoadRequestKind::Skip,
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

TEST(TileLoadRequestPlannerTest, ContentOwnedTerrainQuadtreeDoesNotFallbackToTerrain) {
    TileLoadRequestSnapshot snapshot;
    snapshot.contentProviderOwnsTerrainQuadtree = true;

    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.contentProviderSupportsTile = true;

    EXPECT_EQ(
        TileLoadRequestKind::Content,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, ContentProviderMakesUnloadedTilesRequestable) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.loadState = TileLoadState::Unloaded;

    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.contentProviderSupportsTile = true;

    EXPECT_EQ(
        TileLoadRequestKind::Content,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, ClassifiesUpsampledTerrainAsContentUpsample) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
    snapshot.loadState = TileLoadState::FailedTemporarily;

    EXPECT_EQ(
        TileLoadRequestKind::TerrainContentUpsample,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot = TileLoadRequestSnapshot{};

    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, UpsampledTerrainTakesContentUpsamplePathBeforeProviderChecks) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
    snapshot.loadState = TileLoadState::Unloaded;
    snapshot.hasRenderContent = true;

    EXPECT_EQ(
        TileLoadRequestKind::TerrainContentUpsample,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, ContentOwnedTerrainUpsampleUsesGltfPath) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
    snapshot.contentProviderOwnsTerrainQuadtree = true;
    snapshot.loadState = TileLoadState::Unloaded;

    EXPECT_EQ(
        TileLoadRequestKind::TerrainContentUpsample,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest,
     RasterDetailUpsampleKeepsExplicitUpsampleKind) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.upsampleKind = TileContentUpsampleKind::RasterDetail;
    snapshot.loadState = TileLoadState::Unloaded;

    EXPECT_EQ(
        TileLoadRequestKind::TerrainContentUpsample,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, UpsampleDoesNotRequireHeightmapSurfacePath) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.upsampleKind = TileContentUpsampleKind::TerrainAvailability;
    snapshot.loadState = TileLoadState::Unloaded;

    EXPECT_EQ(
        TileLoadRequestKind::TerrainContentUpsample,
        TileLoadRequestPlanner::classify(snapshot));
}

TEST(TileLoadRequestPlannerTest, SkipsNonRetryableLoadStates) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;

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

TEST(TileLoadRequestPlannerTest, ContentProviderKeepsTemporarilyFailedTilesRetryable) {
    TileLoadRequestSnapshot snapshot;
    snapshot.hasTile = true;
    snapshot.loadState = TileLoadState::FailedTemporarily;

    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.contentProviderSupportsTile = true;

    EXPECT_EQ(
        TileLoadRequestKind::Content,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.hasRenderContent = true;

    EXPECT_EQ(
        TileLoadRequestKind::Content,
        TileLoadRequestPlanner::classify(snapshot));

    snapshot.loadState = TileLoadState::Failed;

    EXPECT_EQ(
        TileLoadRequestKind::Skip,
        TileLoadRequestPlanner::classify(snapshot));
}
