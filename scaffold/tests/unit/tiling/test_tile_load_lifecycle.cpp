#include <gtest/gtest.h>

#include "earth_engine/tiling/TileLoadLifecycle.h"

#include <memory>
#include <mutex>

using namespace earth_engine;

TEST(TileLoadLifecycleTest, CountsAndFindsPendingWork) {
    TileLoadLifecycle lifecycle;
    CancellationToken token;

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "terrain-request",
            token));
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            TileKey{"test", 0, 0, 0},
            "content-upload",
            TileLoadPriorityGroup::Urgent,
            0.0,
            TileContentLoadResult::failed()});
    }

    TileLoadLifecycleCounts counts = lifecycle.counts();
    EXPECT_EQ(1u, counts.requests.terrainRequests);
    EXPECT_EQ(1u, counts.contentUploads);
    EXPECT_TRUE(lifecycle.containsWorkForAnyCacheKey(
        {"missing", "content-upload"}));
    EXPECT_TRUE(lifecycle.hasPendingWork());

    lifecycle.cancelAndEraseCacheKey("content-upload");

    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("content-upload"));

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("terrain-request");
    }

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadLifecycleTest, EmptyBatchQueryIsNoOp) {
    TileLoadLifecycle lifecycle;
    CancellationToken token;

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "terrain-request",
            token));
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            TileKey{"test", 0, 0, 0},
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileContentLoadResult::empty()});
    }

    EXPECT_FALSE(lifecycle.containsWorkForAnyCacheKey({}));
    EXPECT_FALSE(token.isCancelled());
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("terrain-request"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content-upload"));

    lifecycle.cancelAndEraseCacheKey("terrain-request");
    lifecycle.cancelAndEraseCacheKey("content-upload");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadLifecycleTest, CancelErasesPendingUploads) {
    TileLoadLifecycle lifecycle;
    const TileKey terrainKey{"test", 0, 0, 0};
    const TileKey contentKey{"test", 0, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            terrainKey,
            "terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            std::make_unique<DecodedHeightmap>()});
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            contentKey,
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileContentLoadResult::empty()});
    }

    lifecycle.cancelAndEraseCacheKey("terrain-upload");

    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("terrain-upload"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content-upload"));

    lifecycle.cancelAndEraseCacheKey("content-upload");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadLifecycleTest, CancelErasesClaimedUploads) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey terrainKey{"test", 0, 0, 0};
    const TileKey contentKey{"test", 0, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            terrainKey,
            "terrain-upload",
            TileLoadPriorityGroup::Urgent,
            100.0,
            std::make_unique<DecodedHeightmap>()});
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            contentKey,
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileContentLoadResult::empty()});

        EXPECT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
        EXPECT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("terrain-upload"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content-upload"));

    lifecycle.cancelAndEraseCacheKey("terrain-upload");

    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("terrain-upload"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content-upload"));

    lifecycle.cancelAndEraseCacheKey("content-upload");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadLifecycleTest, CancelIgnoresEmptyCacheKey) {
    TileLoadLifecycle lifecycle;
    CancellationToken requestToken;

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "terrain-request",
            requestToken));
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            TileKey{"test", 0, 0, 0},
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileContentLoadResult::empty()});
    }

    lifecycle.cancelAndEraseCacheKey("");

    EXPECT_FALSE(requestToken.isCancelled());
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("terrain-request"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content-upload"));

    lifecycle.cancelAndEraseCacheKey("terrain-request");
    lifecycle.cancelAndEraseCacheKey("content-upload");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}
