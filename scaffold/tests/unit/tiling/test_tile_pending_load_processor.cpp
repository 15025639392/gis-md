#include <gtest/gtest.h>

#include "earth_engine/tiling/TilePendingLoadProcessor.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace earth_engine;

namespace {

PendingTileLoad terrainTerminal(
    const TileKey& key,
    std::string cacheKey,
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal,
    double priority = 0.0,
    TileLoadStatus status = TileLoadStatus::RetryLater) {
    return PendingTileLoad{
        TileLoadDomain::LegacyHeightmapTerrain,
        key,
        std::move(cacheKey),
        group,
        priority,
        status};
}

PendingTileLoad contentTerminal(
    const TileKey& key,
    std::string cacheKey,
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal,
    double priority = 0.0,
    TileLoadStatus status = TileLoadStatus::Empty) {
    return PendingTileLoad{
        TileLoadDomain::Content,
        key,
        std::move(cacheKey),
        group,
        priority,
        status};
}

PendingTileLoad terrainUpload(
    const TileKey& key,
    std::string cacheKey,
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal,
    double priority = 0.0) {
    return PendingTileLoad{
        TileLoadDomain::LegacyHeightmapTerrain,
        key,
        std::move(cacheKey),
        group,
        priority,
        TileLoadResult::createRenderableTerrain()};
}

PendingTileLoad contentUpload(
    const TileKey& key,
    std::string cacheKey,
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal,
    double priority = 0.0) {
    return PendingTileLoad{
        TileLoadDomain::Content,
        key,
        std::move(cacheKey),
        group,
        priority,
        TileLoadResult::fromContentResult(TileContentLoadResult::failed())};
}

PendingTileLoad gltfTerrainUpload(
    const TileKey& key,
    std::string cacheKey,
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Normal,
    double priority = 0.0) {
    TileContentLoadResult content =
        TileContentLoadResult::render(std::make_unique<GltfModel>());
    content.terrainRenderContent = true;
    return PendingTileLoad{
        TileLoadDomain::GltfTerrain,
        key,
        std::move(cacheKey),
        group,
        priority,
        TileLoadResult::fromContentResult(std::move(content))};
}

std::string labelFor(const char* prefix, const PendingTileLoad& load) {
    return std::string(prefix) +
           (load.domain == TileLoadDomain::Content ? ":content:" :
            load.domain == TileLoadDomain::GltfTerrain ? ":gltf-terrain:"
                                                       : ":terrain:") +
           load.cacheKey;
}

} // namespace

TEST(TilePendingLoadProcessorTest, DrainsTerminalThenBudgetedUploads) {
    TileLoadLifecycle lifecycle;
    const TileKey terrainKey{"test", 1, 0, 0};
    const TileKey contentKey{"test", 1, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerminalResult(
            terrainTerminal(terrainKey, "terrain-terminal"));
        lifecycle.pendingLoads().addTerminalResult(
            contentTerminal(
                contentKey,
                "content-terminal",
                TileLoadPriorityGroup::Urgent));
        lifecycle.pendingLoads().addUpload(
            terrainUpload(terrainKey, "terrain-upload"));
        lifecycle.pendingLoads().addUpload(
            contentUpload(
                contentKey,
                "content-upload",
                TileLoadPriorityGroup::Urgent));
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
            [&events](PendingTileLoad& load) {
                events.push_back(labelFor("terminal", load));
            },
            [&events](PendingTileLoad& load) {
                events.push_back(labelFor("upload", load));
            });

    ASSERT_TRUE(changed);
    ASSERT_EQ(3u, events.size());
    EXPECT_EQ("terminal:content:content-terminal", events[0]);
    EXPECT_EQ("terminal:terrain:terrain-terminal", events[1]);
    EXPECT_EQ("upload:content:content-upload", events[2]);

    const TileLoadLifecycleCounts counts = lifecycle.counts();
    EXPECT_EQ(1u, counts.terrainUploads);
    EXPECT_EQ(0u, counts.contentUploads);
}

TEST(TilePendingLoadProcessorTest, GltfTerrainUsesContentFinalizeLane) {
    TileLoadLifecycle lifecycle;
    const TileKey key{"test", 1, 0, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(
            gltfTerrainUpload(key, "gltf-terrain-upload"));
    }

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<FrameResourceLane> elapsedLanes;
    std::vector<std::string> events;

    const bool changed =
        TilePendingLoadProcessor::processPendingLoads(
            TilePendingLoadProcessorInput{
                lifecycle,
                budget,
                false,
                [&elapsedLanes](FrameResourceLane lane) {
                    elapsedLanes.push_back(lane);
                    return std::optional<double>{0.25};
                }},
            [&events](PendingTileLoad& load) {
                events.push_back(labelFor("terminal", load));
            },
            [&events](PendingTileLoad& load) {
                events.push_back(labelFor("upload", load));
            });

    ASSERT_TRUE(changed);
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("upload:gltf-terrain:gltf-terrain-upload", events[0]);
    ASSERT_EQ(1u, elapsedLanes.size());
    EXPECT_EQ(FrameResourceLane::ContentFinalize, elapsedLanes[0]);

    const TileLoadLifecycleCounts counts = lifecycle.counts();
    EXPECT_EQ(0u, counts.terrainUploads);
    EXPECT_EQ(0u, counts.contentUploads);
    EXPECT_EQ(0u, counts.gltfTerrainUploads);
}

TEST(TilePendingLoadProcessorTest, FinalizeBudgetPreservesUploadPriority) {
    TileLoadLifecycle lifecycle;
    const TileKey lowPriorityKey{"test", 1, 0, 0};
    const TileKey highPriorityKey{"test", 1, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(
            terrainUpload(lowPriorityKey, "low-priority"));
        lifecycle.pendingLoads().addUpload(
            terrainUpload(
                highPriorityKey,
                "high-priority",
                TileLoadPriorityGroup::Urgent,
                100.0));
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
            [](PendingTileLoad&) {},
            [&events](PendingTileLoad& upload) {
                events.push_back(upload.cacheKey);
            });

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
        lifecycle.pendingLoads().addTerminalResult(
            terrainTerminal(firstKey, "first-terminal"));
        lifecycle.pendingLoads().addTerminalResult(
            terrainTerminal(secondKey, "second-terminal"));
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
            [&events](PendingTileLoad& result) {
                events.push_back(result.cacheKey);
            },
            [](PendingTileLoad&) {});

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
        lifecycle.pendingLoads().addTerminalResult(
            terrainTerminal(
                terminalKey,
                "terminal",
                TileLoadPriorityGroup::Urgent));
        lifecycle.pendingLoads().addUpload(
            contentUpload(
                uploadKey,
                "upload",
                TileLoadPriorityGroup::Urgent));
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
            [&events](PendingTileLoad&) {
                events.push_back("terminal");
            },
            [&events](PendingTileLoad&) {
                events.push_back("upload");
            });

    const TileLoadLifecycleCounts counts = lifecycle.counts();
    EXPECT_FALSE(changed);
    EXPECT_TRUE(events.empty());
    EXPECT_EQ(1u, counts.terrainTerminalResults);
    EXPECT_EQ(1u, counts.contentUploads);
}

TEST(TilePendingLoadProcessorTest, InteractionFiltersNonUrgentUploads) {
    TileLoadLifecycle lifecycle;
    const TileKey urgentKey{"test", 1, 0, 0};
    const TileKey normalKey{"test", 1, 1, 0};

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(
            terrainUpload(normalKey, "normal"));
        lifecycle.pendingLoads().addUpload(
            terrainUpload(
                urgentKey,
                "urgent",
                TileLoadPriorityGroup::Urgent,
                100.0));
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
            [](PendingTileLoad&) {},
            [&events](PendingTileLoad& upload) {
                events.push_back(upload.cacheKey);
            });

    ASSERT_TRUE(changed);
    ASSERT_EQ(1u, events.size());
    EXPECT_EQ("urgent", events[0]);
    EXPECT_EQ(1u, lifecycle.counts().terrainUploads);
    EXPECT_TRUE(lifecycle.containsWorkForCacheKey("normal"));
}
