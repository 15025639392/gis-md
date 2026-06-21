#include <gtest/gtest.h>

#include "earth_engine/tiling/TilePendingRequestState.h"

using namespace earth_engine;

TEST(TilePendingRequestStateTest, CountsAndCompletesRequests) {
    TilePendingRequestState state;
    CancellationToken terrainToken;
    CancellationToken contentToken;

    EXPECT_TRUE(state.beginTerrainRequest("terrain", terrainToken));
    EXPECT_TRUE(state.beginContentRequest("content", contentToken));
    EXPECT_FALSE(state.beginTerrainRequest("terrain", CancellationToken{}));

    PendingRequestCounts counts = state.counts();
    EXPECT_EQ(2u, counts.totalRequests);
    EXPECT_EQ(1u, counts.terrainRequests);
    EXPECT_EQ(1u, counts.contentRequests);

    state.completeContentRequest("content");
    EXPECT_FALSE(state.contains("content"));
    EXPECT_TRUE(state.contains("terrain"));

    state.completeTerrainRequest("terrain");
    EXPECT_TRUE(state.empty());

    EXPECT_TRUE(state.beginContentRequest(
        "content-mismatched-complete",
        CancellationToken{}));
    state.completeTerrainRequest("content-mismatched-complete");
    counts = state.counts();
    EXPECT_TRUE(state.empty());
    EXPECT_EQ(0u, counts.contentRequests);
    EXPECT_EQ(0u, counts.totalRequests);

    EXPECT_TRUE(state.beginTerrainRequest(
        "terrain-mismatched-complete",
        CancellationToken{}));
    state.completeContentRequest("terrain-mismatched-complete");
    counts = state.counts();
    EXPECT_TRUE(state.empty());
    EXPECT_EQ(0u, counts.terrainRequests);
    EXPECT_EQ(0u, counts.totalRequests);
}

TEST(TilePendingRequestStateTest, RejectsEmptyCacheKeys) {
    TilePendingRequestState state;
    CancellationToken terrainToken;
    CancellationToken contentToken;

    EXPECT_FALSE(state.beginTerrainRequest("", terrainToken));
    EXPECT_FALSE(state.beginContentRequest("", contentToken));
    EXPECT_FALSE(state.contains(""));
    EXPECT_TRUE(state.empty());
    EXPECT_EQ(0u, state.totalRequestCount());

    const PendingRequestCounts counts = state.counts();
    EXPECT_EQ(0u, counts.terrainRequests);
    EXPECT_EQ(0u, counts.contentRequests);
    EXPECT_EQ(0u, counts.totalRequests);
}

TEST(TilePendingRequestStateTest, CancelsAndRejectsDuringDestroy) {
    TilePendingRequestState state;
    CancellationToken terrainToken;
    CancellationToken contentToken;

    EXPECT_TRUE(state.beginTerrainRequest("terrain", terrainToken));
    EXPECT_TRUE(state.beginContentRequest("content", contentToken));

    state.markDestroyingAndCancelRequests();

    EXPECT_TRUE(state.destroying());
    EXPECT_TRUE(terrainToken.isCancelled());
    EXPECT_TRUE(contentToken.isCancelled());

    PendingRequestCounts counts = state.counts();
    EXPECT_EQ(1u, counts.terrainRequests);
    EXPECT_EQ(1u, counts.contentRequests);
    EXPECT_EQ(2u, counts.totalRequests);
    EXPECT_FALSE(state.beginContentRequest("content", CancellationToken{}));

    state.completeContentRequest("content");
    state.completeTerrainRequest("terrain");
    EXPECT_TRUE(state.empty());

    state.clearAfterCallbacksComplete();

    EXPECT_FALSE(state.destroying());
    EXPECT_TRUE(state.beginTerrainRequest(
        "terrain-after-destroy",
        CancellationToken{}));
    state.completeTerrainRequest("terrain-after-destroy");
}

TEST(TilePendingRequestStateTest, ClearWaitsForCallbackDrain) {
    TilePendingRequestState state;
    CancellationToken terrainToken;

    EXPECT_TRUE(state.beginTerrainRequest("terrain", terrainToken));

    state.markDestroyingAndCancelRequests();
    state.clearAfterCallbacksComplete();

    PendingRequestCounts counts = state.counts();
    EXPECT_TRUE(state.destroying());
    EXPECT_TRUE(state.contains("terrain"));
    EXPECT_EQ(1u, counts.terrainRequests);
    EXPECT_EQ(1u, counts.totalRequests);
    EXPECT_FALSE(state.beginContentRequest(
        "content-after-early-clear",
        CancellationToken{}));

    state.completeTerrainRequest("terrain");
    state.clearAfterCallbacksComplete();

    EXPECT_FALSE(state.destroying());
    EXPECT_TRUE(state.empty());
}

TEST(TilePendingRequestStateTest, DestroyMarkIsIdempotent) {
    TilePendingRequestState state;
    CancellationToken terrainToken;
    CancellationToken contentToken;

    EXPECT_TRUE(state.beginTerrainRequest("terrain", terrainToken));
    EXPECT_TRUE(state.beginContentRequest("content", contentToken));

    state.markDestroyingAndCancelRequests();
    state.markDestroyingAndCancelRequests();

    const PendingRequestCounts counts = state.counts();
    EXPECT_TRUE(state.destroying());
    EXPECT_TRUE(terrainToken.isCancelled());
    EXPECT_TRUE(contentToken.isCancelled());
    EXPECT_TRUE(state.contains("terrain"));
    EXPECT_TRUE(state.contains("content"));
    EXPECT_EQ(1u, counts.terrainRequests);
    EXPECT_EQ(1u, counts.contentRequests);
    EXPECT_EQ(2u, counts.totalRequests);
    EXPECT_FALSE(state.beginTerrainRequest(
        "terrain-after-repeat",
        CancellationToken{}));

    state.completeTerrainRequest("terrain");
    state.completeContentRequest("content");
    state.clearAfterCallbacksComplete();

    EXPECT_FALSE(state.destroying());
    EXPECT_TRUE(state.empty());
}

TEST(TilePendingRequestStateTest, CancelIgnoresUnknownKeys) {
    TilePendingRequestState state;
    CancellationToken terrainToken;
    CancellationToken contentToken;

    EXPECT_TRUE(state.beginTerrainRequest("terrain", terrainToken));
    EXPECT_TRUE(state.beginContentRequest("content", contentToken));

    state.cancelAndErase("missing");

    const PendingRequestCounts counts = state.counts();
    EXPECT_FALSE(terrainToken.isCancelled());
    EXPECT_FALSE(contentToken.isCancelled());
    EXPECT_TRUE(state.contains("terrain"));
    EXPECT_TRUE(state.contains("content"));
    EXPECT_EQ(1u, counts.terrainRequests);
    EXPECT_EQ(1u, counts.contentRequests);
    EXPECT_EQ(2u, counts.totalRequests);

    state.cancelAndErase("terrain");
    state.cancelAndErase("content");

    EXPECT_TRUE(state.empty());
}
