#include <gtest/gtest.h>

#include "earth_engine/tiling/TilePendingLoadQueue.h"

using namespace earth_engine;

TEST(TilePendingLoadQueueTest, UsesSharedUploadPriorityOrder) {
    TilePendingLoadQueue queue;
    const TileKey terrainKey{"test", 1, 0, 0};
    const TileKey contentKey{"test", 1, 1, 0};

    queue.addTerrainUpload(PendingTerrainUpload{
        terrainKey,
        "terrain",
        TileLoadPriorityGroup::Normal,
        1.0,
        nullptr});
    queue.addContentUpload(PendingContentUpload{
        contentKey,
        "content",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileContentLoadResult::failed()});

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingLoadFinalize> first =
        queue.takeHighestPriorityUpload(false, budget);

    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(PendingLoadFinalizeKind::Content, first->kind);
    EXPECT_EQ(0u, queue.contentUploadCount());
    EXPECT_EQ(1u, queue.terrainUploadCount());
}

TEST(TilePendingLoadQueueTest, FiltersNonUrgentUploadsDuringInteraction) {
    TilePendingLoadQueue queue;
    const TileKey normalKey{"test", 1, 0, 0};
    const TileKey urgentKey{"test", 1, 1, 0};

    queue.addTerrainUpload(PendingTerrainUpload{
        normalKey,
        "normal",
        TileLoadPriorityGroup::Normal,
        0.0,
        nullptr});
    queue.addTerrainUpload(PendingTerrainUpload{
        urgentKey,
        "urgent",
        TileLoadPriorityGroup::Urgent,
        100.0,
        nullptr});

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(2, config);

    std::optional<PendingLoadFinalize> first =
        queue.takeHighestPriorityUpload(true, budget);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->terrainUpload.has_value());
    EXPECT_EQ(PendingLoadFinalizeKind::Terrain, first->kind);
    EXPECT_EQ("urgent", first->terrainUpload->cacheKey);

    std::optional<PendingLoadFinalize> second =
        queue.takeHighestPriorityUpload(true, budget);

    EXPECT_FALSE(second.has_value());
    EXPECT_EQ(1u, queue.terrainUploadCount());
    EXPECT_TRUE(queue.containsCacheKey("normal"));
}

TEST(TilePendingLoadQueueTest, KeepsUploadWhenFinalizeBudgetBlocks) {
    TilePendingLoadQueue queue;
    const TileKey key{"test", 1, 0, 0};

    queue.addContentUpload(PendingContentUpload{
        key,
        "content",
        TileLoadPriorityGroup::Urgent,
        0.0,
        TileContentLoadResult::failed()});

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxMainThreadFinalizesPerFrame = 0;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(1, blockedConfig);

    std::optional<PendingLoadFinalize> blocked =
        queue.takeHighestPriorityUpload(false, blockedBudget);

    EXPECT_FALSE(blocked.has_value());
    EXPECT_EQ(1u, queue.contentUploadCount());
    EXPECT_TRUE(queue.containsCacheKey("content"));

    FrameResourceBudgetConfig retryConfig;
    retryConfig.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget retryBudget;
    retryBudget.beginFrame(2, retryConfig);

    std::optional<PendingLoadFinalize> retry =
        queue.takeHighestPriorityUpload(false, retryBudget);

    ASSERT_TRUE(retry.has_value());
    ASSERT_TRUE(retry->contentUpload.has_value());
    EXPECT_EQ(PendingLoadFinalizeKind::Content, retry->kind);
    EXPECT_EQ("content", retry->contentUpload->cacheKey);
    EXPECT_EQ(0u, queue.contentUploadCount());
}

TEST(TilePendingLoadQueueTest, DeduplicatesUploadsByKind) {
    TilePendingLoadQueue queue;
    const TileKey firstKey{"test", 1, 0, 0};
    const TileKey secondKey{"test", 1, 1, 0};

    queue.addTerrainUpload(PendingTerrainUpload{
        firstKey,
        "terrain",
        TileLoadPriorityGroup::Normal,
        1.0,
        nullptr});
    queue.addTerrainUpload(PendingTerrainUpload{
        secondKey,
        "terrain",
        TileLoadPriorityGroup::Urgent,
        100.0,
        nullptr});
    queue.addContentUpload(PendingContentUpload{
        firstKey,
        "content",
        TileLoadPriorityGroup::Normal,
        1.0,
        TileContentLoadResult::empty()});
    queue.addContentUpload(PendingContentUpload{
        secondKey,
        "content",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileContentLoadResult::empty()});

    EXPECT_EQ(1u, queue.terrainUploadCount());
    EXPECT_EQ(1u, queue.contentUploadCount());

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingLoadFinalize> first =
        queue.takeHighestPriorityUpload(false, budget);
    std::optional<PendingLoadFinalize> second =
        queue.takeHighestPriorityUpload(false, budget);
    std::optional<PendingLoadFinalize> third =
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

    queue.addTerrainTerminalResult(PendingTerrainTerminalResult{
        lowKey,
        "low",
        TileLoadPriorityGroup::Normal,
        0.0,
        TerrainTileLoadStatus::Failed});
    queue.addTerrainTerminalResult(PendingTerrainTerminalResult{
        highKey,
        "high",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TerrainTileLoadStatus::RetryLater});
    queue.addContentTerminalResult(PendingContentTerminalResult{
        contentKey,
        "content",
        TileLoadPriorityGroup::Urgent,
        50.0,
        TileContentLoadStatus::Empty});

    FrameResourceBudgetConfig config;
    config.maxTerminalStateTransitionsPerFrame = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingTerminalResult> first =
        queue.takeHighestPriorityTerminalResult(budget);

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(first->contentResult.has_value());
    EXPECT_EQ(PendingTerminalResultKind::Content, first->kind);
    EXPECT_EQ("content", first->contentResult->cacheKey);

    std::optional<PendingTerminalResult> second =
        queue.takeHighestPriorityTerminalResult(budget);

    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(second->terrainResult.has_value());
    EXPECT_EQ(PendingTerminalResultKind::Terrain, second->kind);
    EXPECT_EQ("high", second->terrainResult->cacheKey);
    EXPECT_EQ(1u, queue.terrainTerminalResultCount());
    EXPECT_EQ(0u, queue.contentTerminalResultCount());
}

TEST(TilePendingLoadQueueTest, DeduplicatesTerminalResultsByKind) {
    TilePendingLoadQueue queue;
    const TileKey firstKey{"test", 1, 0, 0};
    const TileKey secondKey{"test", 1, 1, 0};

    queue.addTerrainTerminalResult(PendingTerrainTerminalResult{
        firstKey,
        "terrain-terminal",
        TileLoadPriorityGroup::Normal,
        1.0,
        TerrainTileLoadStatus::RetryLater});
    queue.addTerrainTerminalResult(PendingTerrainTerminalResult{
        secondKey,
        "terrain-terminal",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TerrainTileLoadStatus::Cancelled});
    queue.addContentTerminalResult(PendingContentTerminalResult{
        firstKey,
        "content-terminal",
        TileLoadPriorityGroup::Normal,
        1.0,
        TileContentLoadStatus::RetryLater});
    queue.addContentTerminalResult(PendingContentTerminalResult{
        secondKey,
        "content-terminal",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileContentLoadStatus::Cancelled});

    EXPECT_EQ(1u, queue.terrainTerminalResultCount());
    EXPECT_EQ(1u, queue.contentTerminalResultCount());

    FrameResourceBudgetConfig config;
    config.maxTerminalStateTransitionsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingTerminalResult> first =
        queue.takeHighestPriorityTerminalResult(budget);
    std::optional<PendingTerminalResult> second =
        queue.takeHighestPriorityTerminalResult(budget);
    std::optional<PendingTerminalResult> third =
        queue.takeHighestPriorityTerminalResult(budget);

    EXPECT_TRUE(first.has_value());
    EXPECT_TRUE(second.has_value());
    EXPECT_FALSE(third.has_value());
}

TEST(TilePendingLoadQueueTest, KeepsOneResultShapePerKind) {
    TilePendingLoadQueue queue;
    const TileKey terrainKey{"test", 1, 0, 0};
    const TileKey contentKey{"test", 1, 1, 0};

    queue.addTerrainUpload(PendingTerrainUpload{
        terrainKey,
        "terrain",
        TileLoadPriorityGroup::Normal,
        1.0,
        nullptr});
    queue.addTerrainTerminalResult(PendingTerrainTerminalResult{
        terrainKey,
        "terrain",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TerrainTileLoadStatus::RetryLater});
    queue.addContentTerminalResult(PendingContentTerminalResult{
        contentKey,
        "content",
        TileLoadPriorityGroup::Normal,
        1.0,
        TileContentLoadStatus::RetryLater});
    queue.addContentUpload(PendingContentUpload{
        contentKey,
        "content",
        TileLoadPriorityGroup::Urgent,
        100.0,
        TileContentLoadResult::empty()});

    EXPECT_EQ(1u, queue.terrainUploadCount());
    EXPECT_EQ(0u, queue.terrainTerminalResultCount());
    EXPECT_EQ(0u, queue.contentUploadCount());
    EXPECT_EQ(1u, queue.contentTerminalResultCount());

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    config.maxTerminalStateTransitionsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    std::optional<PendingLoadFinalize> upload =
        queue.takeHighestPriorityUpload(false, budget);
    std::optional<PendingTerminalResult> terminal =
        queue.takeHighestPriorityTerminalResult(budget);
    std::optional<PendingLoadFinalize> extraUpload =
        queue.takeHighestPriorityUpload(false, budget);
    std::optional<PendingTerminalResult> extraTerminal =
        queue.takeHighestPriorityTerminalResult(budget);

    EXPECT_TRUE(upload.has_value());
    EXPECT_TRUE(terminal.has_value());
    EXPECT_FALSE(extraUpload.has_value());
    EXPECT_FALSE(extraTerminal.has_value());
}

TEST(TilePendingLoadQueueTest, KeepsTerminalResultWhenBudgetBlocks) {
    TilePendingLoadQueue queue;
    const TileKey key{"test", 1, 0, 0};

    queue.addTerrainTerminalResult(PendingTerrainTerminalResult{
        key,
        "terminal",
        TileLoadPriorityGroup::Urgent,
        0.0,
        TerrainTileLoadStatus::RetryLater});

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxTerminalStateTransitionsPerFrame = 0;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(1, blockedConfig);

    std::optional<PendingTerminalResult> blocked =
        queue.takeHighestPriorityTerminalResult(blockedBudget);

    EXPECT_FALSE(blocked.has_value());
    EXPECT_EQ(1u, queue.terrainTerminalResultCount());
    EXPECT_TRUE(queue.containsCacheKey("terminal"));

    FrameResourceBudgetConfig retryConfig;
    retryConfig.maxTerminalStateTransitionsPerFrame = 1;
    FrameResourceBudget retryBudget;
    retryBudget.beginFrame(2, retryConfig);

    std::optional<PendingTerminalResult> retry =
        queue.takeHighestPriorityTerminalResult(retryBudget);

    ASSERT_TRUE(retry.has_value());
    ASSERT_TRUE(retry->terrainResult.has_value());
    EXPECT_EQ(PendingTerminalResultKind::Terrain, retry->kind);
    EXPECT_EQ("terminal", retry->terrainResult->cacheKey);
    EXPECT_EQ(0u, queue.terrainTerminalResultCount());
}
