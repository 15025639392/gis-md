#include <gtest/gtest.h>

#include "earth_engine/tiling/TilePendingLoadProcessor.h"

#include <mutex>
#include <string>
#include <vector>

using namespace earth_engine;

TEST(TilePendingLoadProcessorTest, DrainsTerminalThenBudgetedUploads) {
    TileLoadLifecycle lifecycle;
    const TileKey terrainKey{"test", 1, 0, 0};
    const TileKey contentKey{"test", 1, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainTerminalResult(
            PendingTerrainTerminalResult{
                terrainKey,
                "terrain-terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TerrainTileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addContentTerminalResult(
            PendingContentTerminalResult{
                contentKey,
                "content-terminal",
                TileLoadPriorityGroup::Urgent,
                0.0,
                TileContentLoadStatus::Empty});
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            terrainKey,
            "terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            nullptr});
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            contentKey,
            "content-upload",
            TileLoadPriorityGroup::Urgent,
            0.0,
            TileContentLoadResult::failed()});
    }

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<std::string> events;

    const bool changed =
        TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                lifecycle,
                budget,
                false,
                {}},
            [&events](const PendingTerrainTerminalResult&) {
                events.push_back("terrain-terminal");
            },
            [&events](const PendingContentTerminalResult&) {
                events.push_back("content-terminal");
            },
            [&events](PendingTerrainUpload&) {
                events.push_back("terrain-upload");
            },
            [&events](PendingContentUpload&) {
                events.push_back("content-upload");
            });

    ASSERT_TRUE(changed);
    ASSERT_EQ(3u, events.size());
    EXPECT_EQ("content-terminal", events[0]);
    EXPECT_EQ("terrain-terminal", events[1]);
    EXPECT_EQ("content-upload", events[2]);

    const TileLoadLifecycleCounts counts = lifecycle.counts();
    EXPECT_EQ(1u, counts.terrainUploads);
    EXPECT_EQ(0u, counts.contentUploads);
}

TEST(TilePendingLoadProcessorTest, BudgetsTerminalResults) {
    TileLoadLifecycle lifecycle;
    const TileKey firstKey{"test", 1, 0, 0};
    const TileKey secondKey{"test", 1, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainTerminalResult(
            PendingTerrainTerminalResult{
                firstKey,
                "first-terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TerrainTileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addTerrainTerminalResult(
            PendingTerrainTerminalResult{
                secondKey,
                "second-terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TerrainTileLoadStatus::RetryLater});
    }

    FrameResourceBudgetConfig config;
    config.maxTerminalStateTransitionsPerFrame = 1;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<std::string> events;

    const bool changed =
        TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                lifecycle,
                budget,
                false,
                {}},
            [&events](const PendingTerrainTerminalResult& result) {
                events.push_back(result.cacheKey);
            },
            [](const PendingContentTerminalResult&) {},
            [](PendingTerrainUpload&) {},
            [](PendingContentUpload&) {});

    ASSERT_TRUE(changed);
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("first-terminal", events[0]);
    EXPECT_EQ(1u, lifecycle.counts().terrainTerminalResults);
}

TEST(TilePendingLoadProcessorTest, ReportsUnchangedWhenBudgetBlocksAllWork) {
    TileLoadLifecycle lifecycle;
    const TileKey terminalKey{"test", 1, 0, 0};
    const TileKey uploadKey{"test", 1, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainTerminalResult(
            PendingTerrainTerminalResult{
                terminalKey,
                "terminal",
                TileLoadPriorityGroup::Urgent,
                0.0,
                TerrainTileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            uploadKey,
            "upload",
            TileLoadPriorityGroup::Urgent,
            0.0,
            TileContentLoadResult::failed()});
    }

    FrameResourceBudgetConfig config;
    config.maxTerminalStateTransitionsPerFrame = 0;
    config.maxMainThreadFinalizesPerFrame = 0;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<std::string> events;

    const bool changed =
        TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                lifecycle,
                budget,
                false,
                {}},
            [&events](const PendingTerrainTerminalResult&) {
                events.push_back("terminal");
            },
            [](const PendingContentTerminalResult&) {},
            [](PendingTerrainUpload&) {},
            [&events](PendingContentUpload&) {
                events.push_back("upload");
            });

    const TileLoadLifecycleCounts counts = lifecycle.counts();
    EXPECT_FALSE(changed);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(1u, counts.terrainTerminalResults);
    EXPECT_EQ(1u, counts.contentUploads);
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("terminal"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("upload"));
}
