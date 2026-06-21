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
                TileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addContentTerminalResult(
            PendingContentTerminalResult{
                contentKey,
                "content-terminal",
                TileLoadPriorityGroup::Urgent,
                0.0,
                TileLoadStatus::Empty});
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

TEST(TilePendingLoadProcessorTest, FinalizeBudgetPreservesUploadPriority) {
    TileLoadLifecycle lifecycle;
    const TileKey lowPriorityKey{"test", 1, 0, 0};
    const TileKey highPriorityKey{"test", 1, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            lowPriorityKey,
            "low-priority",
            TileLoadPriorityGroup::Normal,
            1.0,
            nullptr});
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            highPriorityKey,
            "high-priority",
            TileLoadPriorityGroup::Urgent,
            100.0,
            nullptr});
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
            [](const PendingTerrainTerminalResult&) {},
            [](const PendingContentTerminalResult&) {},
            [&events](PendingTerrainUpload& upload) {
                events.push_back(upload.cacheKey);
            },
            [](PendingContentUpload&) {});

    ASSERT_TRUE(changed);
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("high-priority", events[0]);
    EXPECT_EQ(1u, lifecycle.counts().terrainUploads);
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("low-priority"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("high-priority"));
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
                TileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addTerrainTerminalResult(
            PendingTerrainTerminalResult{
                secondKey,
                "second-terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
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
                TileLoadStatus::RetryLater});
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

TEST(TilePendingLoadProcessorTest,
     CountsTerminalElapsedAgainstMainThreadBudget) {
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
                TileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addTerrainTerminalResult(
            PendingTerrainTerminalResult{
                secondKey,
                "second-terminal",
                TileLoadPriorityGroup::Normal,
                1.0,
                TileLoadStatus::RetryLater});
    }

    FrameResourceBudgetConfig config;
    config.maxTerminalStateTransitionsPerFrame = 4;
    config.maxMainThreadFinalizesPerFrame = 4;
    config.mainThreadTimeMs = 0.5;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<std::string> events;

    const bool changed =
        TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                lifecycle,
                budget,
                false,
                [](FrameResourceLane lane) -> std::optional<double> {
                    return lane == FrameResourceLane::TerminalState
                               ? std::optional<double>{1.0}
                               : std::nullopt;
                }},
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
    EXPECT_GT(budget.mainThreadElapsedMs(), 0.0);
}

TEST(TilePendingLoadProcessorTest, TerminalElapsedStopsUploads) {
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
                10.0,
                TileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            uploadKey,
            "upload",
            TileLoadPriorityGroup::Urgent,
            100.0,
            TileContentLoadResult::failed()});
    }

    FrameResourceBudgetConfig config;
    config.maxTerminalStateTransitionsPerFrame = 4;
    config.maxMainThreadFinalizesPerFrame = 4;
    config.mainThreadTimeMs = 0.5;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<std::string> events;

    const bool changed =
        TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                lifecycle,
                budget,
                false,
                [](FrameResourceLane lane) -> std::optional<double> {
                    return lane == FrameResourceLane::TerminalState
                               ? std::optional<double>{1.0}
                               : std::nullopt;
                }},
            [&events](const PendingTerrainTerminalResult&) {
                events.push_back("terminal");
            },
            [](const PendingContentTerminalResult&) {},
            [](PendingTerrainUpload&) {},
            [&events](PendingContentUpload&) {
                events.push_back("upload");
            });

    ASSERT_TRUE(changed);
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("terminal", events[0]);
    EXPECT_EQ(1u, lifecycle.counts().contentUploads);
    EXPECT_GE(budget.mainThreadElapsedMs(), 1.0);
}

TEST(TilePendingLoadProcessorTest, DrainsTerminalDuringInteraction) {
    TileLoadLifecycle lifecycle;
    const TileKey terminalKey{"test", 1, 0, 0};
    const TileKey uploadKey{"test", 1, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainTerminalResult(
            PendingTerrainTerminalResult{
                terminalKey,
                "terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            uploadKey,
            "upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            nullptr});
    }

    FrameResourceBudgetConfig config;
    config.maxTerminalStateTransitionsPerFrame = 4;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<std::string> events;

    const bool changed =
        TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                lifecycle,
                budget,
                true,
                {}},
            [&events](const PendingTerrainTerminalResult& result) {
                events.push_back(result.cacheKey);
            },
            [](const PendingContentTerminalResult&) {},
            [&events](PendingTerrainUpload& upload) {
                events.push_back(upload.cacheKey);
            },
            [](PendingContentUpload&) {});

    ASSERT_TRUE(changed);
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("terminal", events[0]);
    EXPECT_EQ(0u, lifecycle.counts().terrainTerminalResults);
    EXPECT_EQ(1u, lifecycle.counts().terrainUploads);
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("upload"));
}

TEST(TilePendingLoadProcessorTest, ProcessesUrgentUploadDuringInteraction) {
    TileLoadLifecycle lifecycle;
    const TileKey normalKey{"test", 1, 0, 0};
    const TileKey urgentKey{"test", 1, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            normalKey,
            "normal",
            TileLoadPriorityGroup::Normal,
            0.0,
            nullptr});
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            urgentKey,
            "urgent",
            TileLoadPriorityGroup::Urgent,
            100.0,
            nullptr});
    }

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<std::string> events;

    const bool changed =
        TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                lifecycle,
                budget,
                true,
                {}},
            [](const PendingTerrainTerminalResult&) {},
            [](const PendingContentTerminalResult&) {},
            [&events](PendingTerrainUpload& upload) {
                events.push_back(upload.cacheKey);
            },
            [](PendingContentUpload&) {});

    ASSERT_TRUE(changed);
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("urgent", events[0]);
    EXPECT_EQ(1u, lifecycle.counts().terrainUploads);
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("normal"));
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("urgent"));
}
