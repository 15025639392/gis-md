#include <gtest/gtest.h>

#include "earth_engine/tiling/TileLoadRequestPlanner.h"
#include "earth_engine/tiling/TileRetryBackoffPolicy.h"

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

// ---- 瞬时失败退避策略(TileRetryBackoffPolicy)----

TEST(TileRetryBackoffPolicyTest, BackoffIsExponentialAndCapped) {
    EXPECT_DOUBLE_EQ(500.0, TileRetryBackoffPolicy::backoffMs(1));
    EXPECT_DOUBLE_EQ(1000.0, TileRetryBackoffPolicy::backoffMs(2));
    EXPECT_DOUBLE_EQ(2000.0, TileRetryBackoffPolicy::backoffMs(3));
    EXPECT_DOUBLE_EQ(4000.0, TileRetryBackoffPolicy::backoffMs(4));
    // 封顶 30000ms:第 8 次(500*2^7=64000)已超上限。
    EXPECT_DOUBLE_EQ(TileRetryBackoffPolicy::kCapMs,
                     TileRetryBackoffPolicy::backoffMs(8));
    EXPECT_DOUBLE_EQ(TileRetryBackoffPolicy::kCapMs,
                     TileRetryBackoffPolicy::backoffMs(100));
    // attemptCount<=1 边界。
    EXPECT_DOUBLE_EQ(500.0, TileRetryBackoffPolicy::backoffMs(0));
}

TEST(TileRetryBackoffPolicyTest, RetryDueGatesOnNow) {
    // 失败后:retryNotBefore = now + backoff。
    const double now = 10000.0;
    const double retryNotBefore = now + TileRetryBackoffPolicy::backoffMs(1);
    EXPECT_FALSE(TileRetryBackoffPolicy::isRetryDue(retryNotBefore, now));
    EXPECT_FALSE(
        TileRetryBackoffPolicy::isRetryDue(retryNotBefore, retryNotBefore - 1.0));
    EXPECT_TRUE(TileRetryBackoffPolicy::isRetryDue(retryNotBefore, retryNotBefore));
    EXPECT_TRUE(
        TileRetryBackoffPolicy::isRetryDue(retryNotBefore, retryNotBefore + 1.0));
    // 从未失败(retryNotBefore==0)立即可重试。
    EXPECT_TRUE(TileRetryBackoffPolicy::isRetryDue(0.0, 0.0));
}
