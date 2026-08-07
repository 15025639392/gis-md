#include <gtest/gtest.h>

#include "earth_engine/tiling/TilePendingRequestState.h"

#include "earth_engine/debug/Contracts.h"

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

    EXPECT_TRUE(terrainToken.isCancelled());
    EXPECT_TRUE(contentToken.isCancelled());
    EXPECT_TRUE(state.empty());
    EXPECT_FALSE(state.callbacksDrained());

    CancellationToken replacementToken;
    EXPECT_TRUE(state.beginTerrainRequest(
        "terrain",
        replacementToken));
    state.completeTerrainRequest("terrain", terrainToken);
    EXPECT_TRUE(state.contains("terrain"));
    state.completeContentRequest("content", contentToken);
    EXPECT_FALSE(state.callbacksDrained());
    state.completeTerrainRequest("terrain", replacementToken);
    EXPECT_TRUE(state.callbacksDrained());
}

// ---------------------------------------------------------------------------
// PendingRequestParity 契约:正常成对增删全程零违约,且求值数确实在涨
// (活性证明 —— 零违约的死判定点与零违约的健康路径读数相同)。
// 反例无法从公共 API 构造(这正是契约的意义:它防的是未来改动破坏成对性),
// 开发期已用「临时注释掉一处 erase」验证过判定点会响。
// ---------------------------------------------------------------------------

TEST(TilePendingRequestStateTest, ParityContractStaysGreenAndAlive) {
    using earth_engine::contracts::Id;
    const uint32_t violationsBefore =
        earth_engine::contracts::totalViolations(Id::PendingRequestParity);
    const uint32_t evalsBefore =
        earth_engine::contracts::totalEvaluations(Id::PendingRequestParity);

    TilePendingRequestState state;
    CancellationToken terrainToken;
    CancellationToken contentToken;
    EXPECT_TRUE(state.beginTerrainRequest("terrain", terrainToken));
    EXPECT_TRUE(state.beginContentRequest("content", contentToken));
    state.completeContentRequest("content", contentToken);
    state.cancelAndErase("terrain");
    EXPECT_TRUE(state.empty());

    EXPECT_EQ(0u,
              earth_engine::contracts::totalViolations(Id::PendingRequestParity) -
                  violationsBefore);
    // begin×2 + complete(token 版委托无条件版)×1 + cancel×1 = 至少 4 次求值。
    EXPECT_GE(earth_engine::contracts::totalEvaluations(Id::PendingRequestParity) -
                  evalsBefore,
              4u);
}
