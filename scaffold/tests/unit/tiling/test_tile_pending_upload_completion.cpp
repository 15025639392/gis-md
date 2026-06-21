#include <gtest/gtest.h>

#include "earth_engine/tiling/TilePendingUploadCompletion.h"
#include "earth_engine/tiling/TileLoadLifecycle.h"

#include <mutex>

using namespace earth_engine;

TEST(TilePendingUploadCompletionTest, ClaimedUploadCountsAsWork) {
    TileLoadLifecycle lifecycle;

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            TileKey{"test", 1, 0, 0},
            "terrain",
            TileLoadPriorityGroup::Normal,
            1.0,
            nullptr});
        std::optional<PendingLoadFinalize> upload =
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget);

        ASSERT_TRUE(upload.has_value());
        EXPECT_EQ(PendingLoadFinalizeKind::Terrain, upload->kind);
    }

    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("terrain"));
    EXPECT_TRUE(lifecycle.hasPendingWork());

    TilePendingUploadCompletion::eraseTerrainUpload(lifecycle, "terrain");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TilePendingUploadCompletionTest, ErasesUploadKeys) {
    TileLoadLifecycle lifecycle;

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            TileKey{"test", 1, 0, 0},
            "terrain",
            TileLoadPriorityGroup::Normal,
            1.0,
            nullptr});
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            TileKey{"test", 1, 1, 0},
            "content",
            TileLoadPriorityGroup::Normal,
            2.0,
            TileContentLoadResult::empty()});

        std::optional<PendingLoadFinalize> first =
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget);
        std::optional<PendingLoadFinalize> second =
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget);

        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
    }

    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("terrain"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content"));

    TilePendingUploadCompletion::eraseTerrainUpload(lifecycle, "terrain");

    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("terrain"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content"));

    TilePendingUploadCompletion::eraseContentUpload(lifecycle, "content");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}
