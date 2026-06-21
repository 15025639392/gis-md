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
