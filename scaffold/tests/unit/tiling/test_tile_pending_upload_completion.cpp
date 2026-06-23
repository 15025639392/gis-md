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
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
            TileKey{"test", 1, 0, 0},
            "terrain",
            TileLoadPriorityGroup::Normal,
            1.0,
            TileLoadResult::createRenderableTerrain()});
        std::optional<PendingTileLoad> upload =
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget);

        ASSERT_TRUE(upload.has_value());
        EXPECT_EQ(TileLoadDomain::LegacyHeightmapTerrain, upload->domain);
    }

    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("terrain"));
    EXPECT_TRUE(lifecycle.hasPendingWork());

    TilePendingUploadCompletion::eraseUpload(lifecycle, "terrain");

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
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
            TileKey{"test", 1, 0, 0},
            "terrain",
            TileLoadPriorityGroup::Normal,
            1.0,
            TileLoadResult::createRenderableTerrain()});
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            TileKey{"test", 1, 1, 0},
            "content",
            TileLoadPriorityGroup::Normal,
            2.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});

        std::optional<PendingTileLoad> first =
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget);
        std::optional<PendingTileLoad> second =
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget);

        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
    }

    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("terrain"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content"));

    TilePendingUploadCompletion::eraseUpload(lifecycle, "terrain");

    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("terrain"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content"));

    TilePendingUploadCompletion::eraseUpload(lifecycle, "content");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TilePendingUploadCompletionTest, RejectsDuplicateUploadKeyAcrossKinds) {
    TileLoadLifecycle lifecycle;

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
            TileKey{"test", 1, 0, 0},
            "shared",
            TileLoadPriorityGroup::Normal,
            1.0,
            TileLoadResult::createRenderableTerrain()});
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            TileKey{"test", 1, 0, 0},
            "shared",
            TileLoadPriorityGroup::Normal,
            2.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});

        std::optional<PendingTileLoad> first =
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget);
        std::optional<PendingTileLoad> second =
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget);

        ASSERT_TRUE(first.has_value());
        EXPECT_FALSE(second.has_value());
    }

    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("shared"));

    TilePendingUploadCompletion::eraseUpload(lifecycle, "shared");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}
