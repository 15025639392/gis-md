#include <gtest/gtest.h>

#include "earth_engine/tiling/TilePendingLoadQueue.h"

using namespace earth_engine;

TEST(TilePendingLoadQueueTest, UsesSharedUploadPriorityOrder) {
    TilePendingLoadQueue queue;
    const TileKey terrainKey{"test", 1, 0, 0};
    const TileKey contentKey{"test", 1, 1, 0};

    queue.addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
        terrainKey,
        "terrain",
        TileLoadPriorityGroup::Normal,
        1.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
    queue.addUpload(PendingTileLoad{TileLoadDomain::Content,
        contentKey,
        "content",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileLoadResult::fromContentResult(TileContentLoadResult::failed())});

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingTileLoad> first =
        queue.takeHighestPriorityUpload(false, budget);

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(TileLoadDomain::Content, first->domain);
    EXPECT_EQ(0u, queue.contentUploadCount());
    EXPECT_EQ(1u, queue.gltfTerrainUploadCount());
}

TEST(TilePendingLoadQueueTest, FiltersNonUrgentUploadsDuringInteraction) {
    TilePendingLoadQueue queue;
    const TileKey normalKey{"test", 1, 0, 0};
    const TileKey urgentKey{"test", 1, 1, 0};

    queue.addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
        normalKey,
        "normal",
        TileLoadPriorityGroup::Normal,
        0.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
    queue.addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
        urgentKey,
        "urgent",
        TileLoadPriorityGroup::Urgent,
        100.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(2, config);

    std::optional<PendingTileLoad> first =
        queue.takeHighestPriorityUpload(true, budget);

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(TileLoadDomain::TerrainContent, first->domain);
    EXPECT_EQ("urgent", first->cacheKey);

    std::optional<PendingTileLoad> second =
        queue.takeHighestPriorityUpload(true, budget);

    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(1u, queue.gltfTerrainUploadCount());
    EXPECT_TRUE(queue.containsCacheKey("normal"));
}

TEST(TilePendingLoadQueueTest, KeepsUploadWhenFinalizeBudgetBlocks) {
    TilePendingLoadQueue queue;
    const TileKey key{"test", 1, 0, 0};

    queue.addUpload(PendingTileLoad{TileLoadDomain::Content,
        key,
        "content",
        TileLoadPriorityGroup::Urgent,
        0.0,
        TileLoadResult::fromContentResult(TileContentLoadResult::failed())});

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxMainThreadFinalizesPerFrame = 0;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(1, blockedConfig);

    std::optional<PendingTileLoad> blocked =
        queue.takeHighestPriorityUpload(false, blockedBudget);

    EXPECT_FALSE(blocked.has_value());
    EXPECT_EQ(1u, queue.contentUploadCount());
    EXPECT_TRUE(queue.containsCacheKey("content"));

    FrameResourceBudgetConfig retryConfig;
    retryConfig.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget retryBudget;
    retryBudget.beginFrame(2, retryConfig);

    std::optional<PendingTileLoad> retry =
        queue.takeHighestPriorityUpload(false, retryBudget);

    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(TileLoadDomain::Content, retry->domain);
    EXPECT_EQ("content", retry->cacheKey);
    EXPECT_EQ(0u, queue.contentUploadCount());
}

TEST(TilePendingLoadQueueTest, DeduplicatesUploadsByKind) {
    TilePendingLoadQueue queue;
    const TileKey firstKey{"test", 1, 0, 0};
    const TileKey secondKey{"test", 1, 1, 0};

    queue.addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
        firstKey,
        "terrain",
        TileLoadPriorityGroup::Normal,
        1.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
    queue.addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
        secondKey,
        "terrain",
        TileLoadPriorityGroup::Urgent,
        100.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
    queue.addUpload(PendingTileLoad{TileLoadDomain::Content,
        firstKey,
        "content",
        TileLoadPriorityGroup::Normal,
        1.0,
        TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
    queue.addUpload(PendingTileLoad{TileLoadDomain::Content,
        secondKey,
        "content",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileLoadResult::fromContentResult(TileContentLoadResult::empty())});

    EXPECT_EQ(1u, queue.gltfTerrainUploadCount());
    EXPECT_EQ(1u, queue.contentUploadCount());

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingTileLoad> first =
        queue.takeHighestPriorityUpload(false, budget);
    std::optional<PendingTileLoad> second =
        queue.takeHighestPriorityUpload(false, budget);
    std::optional<PendingTileLoad> third =
        queue.takeHighestPriorityUpload(false, budget);

    EXPECT_TRUE(first.has_value());
    EXPECT_TRUE(second.has_value());
    EXPECT_FALSE(third.has_value());
}

TEST(TilePendingLoadQueueTest, TakesTerminalResultsByPriority) {
    TilePendingLoadQueue queue;
    const TileKey lowKey{"test", 1, 0, 0};
    const TileKey highKey{"test", 1, 1, 0};
    const TileKey contentKey{"test", 1, 1, 1};

    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
        lowKey,
        "low",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadStatus::Failed});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
        highKey,
        "high",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileLoadStatus::RetryLater});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
        contentKey,
        "content",
        TileLoadPriorityGroup::Urgent,
        50.0,
        TileLoadStatus::Empty});

    FrameResourceBudgetConfig config;
    config.maxTerminalStateTransitionsPerFrame = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingTileLoad> first =
        queue.takeHighestPriorityTerminalResult(budget);

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(TileLoadDomain::Content, first->domain);
    EXPECT_EQ("content", first->cacheKey);

    std::optional<PendingTileLoad> second =
        queue.takeHighestPriorityTerminalResult(budget);

    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(TileLoadDomain::TerrainContent, second->domain);
    EXPECT_EQ("high", second->cacheKey);
    EXPECT_EQ(1u, queue.gltfTerrainTerminalResultCount());
    EXPECT_EQ(0u, queue.contentTerminalResultCount());
}

TEST(TilePendingLoadQueueTest, DeduplicatesTerminalResultsByKind) {
    TilePendingLoadQueue queue;
    const TileKey firstKey{"test", 1, 0, 0};
    const TileKey secondKey{"test", 1, 1, 0};

    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
        firstKey,
        "terrain-terminal",
        TileLoadPriorityGroup::Normal,
        1.0,
        TileLoadStatus::RetryLater});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
        secondKey,
        "terrain-terminal",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileLoadStatus::Cancelled});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
        firstKey,
        "content-terminal",
        TileLoadPriorityGroup::Normal,
        1.0,
        TileLoadStatus::RetryLater});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
        secondKey,
        "content-terminal",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileLoadStatus::Cancelled});

    EXPECT_EQ(1u, queue.gltfTerrainTerminalResultCount());
    EXPECT_EQ(1u, queue.contentTerminalResultCount());

    FrameResourceBudgetConfig config;
    config.maxTerminalStateTransitionsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingTileLoad> first =
        queue.takeHighestPriorityTerminalResult(budget);
    std::optional<PendingTileLoad> second =
        queue.takeHighestPriorityTerminalResult(budget);
    std::optional<PendingTileLoad> third =
        queue.takeHighestPriorityTerminalResult(budget);

    EXPECT_TRUE(first.has_value());
    EXPECT_TRUE(second.has_value());
    EXPECT_FALSE(third.has_value());
}

TEST(TilePendingLoadQueueTest, KeepsOneResultShapePerKind) {
    TilePendingLoadQueue queue;
    const TileKey terrainKey{"test", 1, 0, 0};
    const TileKey contentKey{"test", 1, 1, 0};

    queue.addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
        terrainKey,
        "terrain",
        TileLoadPriorityGroup::Normal,
        1.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
        terrainKey,
        "terrain",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileLoadStatus::RetryLater});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
        contentKey,
        "content",
        TileLoadPriorityGroup::Normal,
        1.0,
        TileLoadStatus::RetryLater});
    queue.addUpload(PendingTileLoad{TileLoadDomain::Content,
        contentKey,
        "content",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileLoadResult::fromContentResult(TileContentLoadResult::empty())});

    EXPECT_EQ(1u, queue.gltfTerrainUploadCount());
    EXPECT_EQ(0u, queue.gltfTerrainTerminalResultCount());
    EXPECT_EQ(0u, queue.contentUploadCount());
    EXPECT_EQ(1u, queue.contentTerminalResultCount());

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    config.maxTerminalStateTransitionsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingTileLoad> upload =
        queue.takeHighestPriorityUpload(false, budget);
    std::optional<PendingTileLoad> terminal =
        queue.takeHighestPriorityTerminalResult(budget);
    std::optional<PendingTileLoad> extraUpload =
        queue.takeHighestPriorityUpload(false, budget);
    std::optional<PendingTileLoad> extraTerminal =
        queue.takeHighestPriorityTerminalResult(budget);

    EXPECT_TRUE(upload.has_value());
    EXPECT_TRUE(terminal.has_value());
    EXPECT_FALSE(extraUpload.has_value());
    EXPECT_FALSE(extraTerminal.has_value());
}

TEST(TilePendingLoadQueueTest, KeepsTerminalResultWhenBudgetBlocks) {
    TilePendingLoadQueue queue;
    const TileKey key{"test", 1, 0, 0};

    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
        key,
        "terminal",
        TileLoadPriorityGroup::Urgent,
        0.0,
        TileLoadStatus::RetryLater});

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxTerminalStateTransitionsPerFrame = 0;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(1, blockedConfig);

    std::optional<PendingTileLoad> blocked =
        queue.takeHighestPriorityTerminalResult(blockedBudget);

    EXPECT_FALSE(blocked.has_value());
    EXPECT_EQ(1u, queue.gltfTerrainTerminalResultCount());
    EXPECT_TRUE(queue.containsCacheKey("terminal"));

    FrameResourceBudgetConfig retryConfig;
    retryConfig.maxTerminalStateTransitionsPerFrame = 1;
    FrameResourceBudget retryBudget;
    retryBudget.beginFrame(2, retryConfig);

    std::optional<PendingTileLoad> retry =
        queue.takeHighestPriorityTerminalResult(retryBudget);

    ASSERT_TRUE(retry.has_value());
    EXPECT_EQ(TileLoadDomain::TerrainContent, retry->domain);
    EXPECT_EQ("terminal", retry->cacheKey);
    EXPECT_EQ(0u, queue.gltfTerrainTerminalResultCount());
}

TEST(TilePendingLoadQueueTest, RejectsEmptyCacheKeys) {
    TilePendingLoadQueue queue;
    const TileKey key{"test", 1, 0, 0};

    queue.addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
        key,
        "",
        TileLoadPriorityGroup::Urgent,
        0.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
    queue.addUpload(PendingTileLoad{TileLoadDomain::Content,
        key,
        "",
        TileLoadPriorityGroup::Urgent,
        0.0,
        TileLoadResult::fromContentResult(TileContentLoadResult::failed())});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
        key,
        "",
        TileLoadPriorityGroup::Urgent,
        0.0,
        TileLoadStatus::Failed});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
        key,
        "",
        TileLoadPriorityGroup::Urgent,
        0.0,
        TileLoadStatus::Failed});

    EXPECT_FALSE(queue.hasWork());
    EXPECT_EQ(0u, queue.gltfTerrainUploadCount());
    EXPECT_EQ(0u, queue.contentUploadCount());
    EXPECT_EQ(0u, queue.gltfTerrainTerminalResultCount());
    EXPECT_EQ(0u, queue.contentTerminalResultCount());
    EXPECT_FALSE(queue.containsCacheKey(""));
}

TEST(TilePendingLoadQueueTest, EraseIgnoresUnknownKeys) {
    TilePendingLoadQueue queue;
    const TileKey terrainUploadKey{"test", 1, 0, 0};
    const TileKey contentUploadKey{"test", 1, 1, 0};
    const TileKey terrainTerminalKey{"test", 1, 0, 1};
    const TileKey contentTerminalKey{"test", 1, 1, 1};

    queue.addUpload(PendingTileLoad{TileLoadDomain::TerrainContent,
        terrainUploadKey,
        "terrain-upload",
        TileLoadPriorityGroup::Normal,
        0.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
    queue.addUpload(PendingTileLoad{TileLoadDomain::Content,
        contentUploadKey,
        "content-upload",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::fromContentResult(TileContentLoadResult::failed())});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::TerrainContent,
        terrainTerminalKey,
        "terrain-terminal",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadStatus::RetryLater});
    queue.addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
        contentTerminalKey,
        "content-terminal",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadStatus::RetryLater});

    queue.eraseCacheKey("missing");

    EXPECT_TRUE(queue.containsCacheKey("terrain-upload"));
    EXPECT_TRUE(queue.containsCacheKey("content-upload"));
    EXPECT_TRUE(queue.containsCacheKey("terrain-terminal"));
    EXPECT_TRUE(queue.containsCacheKey("content-terminal"));
    EXPECT_EQ(1u, queue.gltfTerrainUploadCount());
    EXPECT_EQ(1u, queue.contentUploadCount());
    EXPECT_EQ(1u, queue.gltfTerrainTerminalResultCount());
    EXPECT_EQ(1u, queue.contentTerminalResultCount());
}
