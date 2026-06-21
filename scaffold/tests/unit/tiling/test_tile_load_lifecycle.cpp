#include <gtest/gtest.h>

#include "earth_engine/tiling/TileLoadLifecycle.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

using namespace earth_engine;

TEST(TileLoadLifecycleTest, CountsAndFindsPendingWork) {
    TileLoadLifecycle lifecycle;
    CancellationToken token;

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "terrain-request",
            token));
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            TileKey{"test", 0, 0, 0},
            "content-upload",
            TileLoadPriorityGroup::Urgent,
            0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::failed())});
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
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            TileKey{"test", 0, 0, 0},
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
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
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Terrain,
            terrainKey,
            "terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain(std::make_unique<DecodedHeightmap>())});
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            contentKey,
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
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
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Terrain,
            terrainKey,
            "terrain-upload",
            TileLoadPriorityGroup::Urgent,
            100.0,
            TileLoadResult::createRenderableTerrain(std::make_unique<DecodedHeightmap>())});
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            contentKey,
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});

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
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            TileKey{"test", 0, 0, 0},
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
    }

    lifecycle.cancelAndEraseCacheKey("");

    EXPECT_FALSE(requestToken.isCancelled());
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("terrain-request"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content-upload"));

    lifecycle.cancelAndEraseCacheKey("terrain-request");
    lifecycle.cancelAndEraseCacheKey("content-upload");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadLifecycleTest, CancelErasesTerminalResults) {
    TileLoadLifecycle lifecycle;
    const TileKey terrainKey{"test", 0, 0, 0};
    const TileKey contentKey{"test", 0, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::Terrain,
                terrainKey,
                "terrain-terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
                contentKey,
                "content-terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
    }

    lifecycle.cancelAndEraseCacheKey("terrain-terminal");

    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("terrain-terminal"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content-terminal"));

    lifecycle.cancelAndEraseCacheKey("content-terminal");

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadLifecycleTest, CancelErasesActiveRequests) {
    TileLoadLifecycle lifecycle;
    CancellationToken terrainToken;
    CancellationToken contentToken;
    CancellationToken keptToken;

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        EXPECT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "terrain-request",
            terrainToken));
        EXPECT_TRUE(lifecycle.requestState().beginContentRequest(
            "content-request",
            contentToken));
        EXPECT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "kept-request",
            keptToken));
    }

    lifecycle.cancelAndEraseCacheKey("terrain-request");

    EXPECT_TRUE(terrainToken.isCancelled());
    EXPECT_FALSE(contentToken.isCancelled());
    EXPECT_FALSE(keptToken.isCancelled());
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("terrain-request"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content-request"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("kept-request"));

    lifecycle.cancelAndEraseCacheKey("content-request");

    EXPECT_TRUE(contentToken.isCancelled());
    EXPECT_FALSE(keptToken.isCancelled());
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("content-request"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("kept-request"));

    lifecycle.cancelAndEraseCacheKey("kept-request");

    EXPECT_TRUE(keptToken.isCancelled());
    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadLifecycleTest, DestroyWithoutRequestsReturnsImmediately) {
    TileLoadLifecycle lifecycle;

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Terrain,
            TileKey{"test", 0, 0, 0},
            "terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
                TileKey{"test", 0, 1, 0},
                "content-terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
    }

    lifecycle.markDestroyingCancelAndWait();

    EXPECT_FALSE(lifecycle.hasPendingWork());
    EXPECT_EQ(0, lifecycle.pendingRequestCount());
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        EXPECT_FALSE(lifecycle.requestState().destroying());
    }
}

TEST(TileLoadLifecycleTest, DestroyClearsClaimedUploadKeys) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            TileKey{"test", 0, 0, 0},
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
        std::optional<PendingTileLoad> upload =
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget);

        ASSERT_TRUE(upload.has_value());
        EXPECT_EQ(TileLoadDomain::Content, upload->domain);
    }

    EXPECT_TRUE(lifecycle.hasPendingWork());
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("content-upload"));

    lifecycle.markDestroyingCancelAndWait();

    EXPECT_FALSE(lifecycle.hasPendingWork());
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("content-upload"));
}

TEST(TileLoadLifecycleTest, DestroyCancelsAndWaitsForCallbacks) {
    TileLoadLifecycle lifecycle;
    CancellationToken token;

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        ASSERT_TRUE(lifecycle.requestState().beginTerrainRequest(
            "terrain",
            token));
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Terrain,
            TileKey{"test", 0, 0, 0},
            "terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
                TileKey{"test", 0, 1, 0},
                "content-terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
    }

    std::atomic<bool> destroyReturned{false};
    std::thread destroyThread([&]() {
        lifecycle.markDestroyingCancelAndWait();
        destroyReturned.store(true);
    });

    for (int i = 0; i < 200 && !token.isCancelled(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    EXPECT_TRUE(token.isCancelled());
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("terrain-upload"));
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("content-terminal"));
    EXPECT_FALSE(destroyReturned.load());

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("terrain");
    }
    lifecycle.condition().notify_all();
    destroyThread.join();

    EXPECT_TRUE(destroyReturned.load());
    EXPECT_FALSE(lifecycle.hasPendingWork());
}
