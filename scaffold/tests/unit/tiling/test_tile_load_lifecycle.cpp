#include <gtest/gtest.h>

#include "earth_engine/tiling/TileLoadLifecycle.h"

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
