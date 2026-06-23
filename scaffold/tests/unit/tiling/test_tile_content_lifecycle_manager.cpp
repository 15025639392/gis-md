#include <gtest/gtest.h>

#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileContentLifecycleManager.h"
#include "earth_engine/tiling/TilePendingUploadCompletion.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using namespace earth_engine;

TEST(TileContentLifecycleManagerTest, OwnsLifecycleState) {
    TileContentLifecycleManager manager;
    CancellationToken token;

    {
        std::lock_guard<std::mutex> lock(manager.loadLifecycle().mutex());
        ASSERT_TRUE(manager.loadLifecycle().requestState().beginTerrainRequest(
            "terrain",
            token));
    }

    EXPECT_EQ(1, manager.pendingRequests());
    EXPECT_TRUE(manager.hasPendingWork());

    std::atomic<bool> shutdownReturned{false};
    std::thread shutdownThread([&]() {
        manager.shutdown();
        shutdownReturned.store(true);
    });

    for (int i = 0; i < 200 && !token.isCancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(token.isCancelled());

    {
        std::lock_guard<std::mutex> lock(manager.loadLifecycle().mutex());
        manager.loadLifecycle()
            .requestState()
            .completeTerrainRequest("terrain");
    }
    manager.loadLifecycle().condition().notify_all();
    shutdownThread.join();

    EXPECT_TRUE(shutdownReturned.load());
    EXPECT_EQ(0, manager.pendingRequests());
    EXPECT_FALSE(manager.hasPendingWork());
}

TEST(TileContentLifecycleManagerTest, ExposesClaimedUploadWork) {
    TileContentLifecycleManager manager;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    {
        std::lock_guard<std::mutex> lock(manager.loadLifecycle().mutex());
        manager.loadLifecycle().pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
                TileKey{"test", 0, 0, 0},
                "content-upload",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadResult::fromContentResult(
                    TileContentLoadResult::empty())});

        EXPECT_TRUE(manager.loadLifecycle()
                        .pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    EXPECT_TRUE(manager.hasPendingWork());
    EXPECT_TRUE(manager.loadLifecycle().containsWorkForCacheKey(
        "content-upload"));

    TilePendingUploadCompletion::eraseUpload(
        manager.loadLifecycle(),
        "content-upload");

    EXPECT_FALSE(manager.hasPendingWork());
}

TEST(TileContentLifecycleManagerTest, ShutdownClearsClaimedUploadWork) {
    TileContentLifecycleManager manager;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    {
        std::lock_guard<std::mutex> lock(manager.loadLifecycle().mutex());
        manager.loadLifecycle().pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::LegacyTerrain,
                TileKey{"test", 0, 0, 0},
                "terrain-upload",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadResult::createRenderableTerrain()});

        EXPECT_TRUE(manager.loadLifecycle()
                        .pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    EXPECT_TRUE(manager.hasPendingWork());
    EXPECT_TRUE(manager.loadLifecycle().containsWorkForCacheKey(
        "terrain-upload"));

    manager.shutdown();

    EXPECT_FALSE(manager.hasPendingWork());
    EXPECT_FALSE(manager.loadLifecycle().containsWorkForCacheKey(
        "terrain-upload"));
}
