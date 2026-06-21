#include <gtest/gtest.h>

#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileLoadLifecycle.h"
#include "earth_engine/tiling/TileSubtreeTraversal.h"
#include "earth_engine/tiling/TileSubtreeWorkTracker.h"

#include <mutex>
#include <string>
#include <vector>

using namespace earth_engine;

namespace {

std::string simpleCacheKey(const TileKey& key) {
    return key.schemeId + ":" +
           std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" +
           std::to_string(key.y);
}

struct SubtreeFixture {
    TilesetTile root{TileKey{"test", 0, 0, 0}, Rectangle{}};
    TilesetTile first{TileKey{"test", 1, 0, 0}, Rectangle{}, &root};
    TilesetTile second{TileKey{"test", 1, 1, 0}, Rectangle{}, &root};
    TilesetTile grandchild{TileKey{"test", 2, 3, 1}, Rectangle{}, &first};

    SubtreeFixture() {
        root.children.push_back(&first);
        root.children.push_back(nullptr);
        root.children.push_back(&second);
        first.children.push_back(&grandchild);
    }
};

} // namespace

TEST(TileSubtreeTraversal, CollectsRootAndNonNullDescendantsInStackOrder) {
    SubtreeFixture fixture;

    const std::vector<const TilesetTile*> tiles =
        TileSubtreeTraversal::collectTiles(fixture.root);

    ASSERT_EQ(tiles.size(), 4);
    EXPECT_EQ(tiles[0], &fixture.root);
    EXPECT_EQ(tiles[1], &fixture.second);
    EXPECT_EQ(tiles[2], &fixture.first);
    EXPECT_EQ(tiles[3], &fixture.grandchild);
}

TEST(TileSubtreeTraversal, MapsCollectedTilesToCacheKeys) {
    SubtreeFixture fixture;

    const std::vector<std::string> keys =
        TileSubtreeTraversal::collectCacheKeys(
            fixture.root,
            simpleCacheKey);

    ASSERT_EQ(keys.size(), 4);
    EXPECT_EQ(keys[0], "test:0:0:0");
    EXPECT_EQ(keys[1], "test:1:1:0");
    EXPECT_EQ(keys[2], "test:1:0:0");
    EXPECT_EQ(keys[3], "test:2:3:1");
}

TEST(TileSubtreeTraversal, BuildsDescendantRemovalPlanWithoutRoot) {
    SubtreeFixture fixture;

    const std::vector<TileSubtreeRemovalEntry> removalPlan =
        TileSubtreeTraversal::collectDescendantsForRemoval(
            fixture.root,
            simpleCacheKey);

    ASSERT_EQ(removalPlan.size(), 3);
    EXPECT_EQ(removalPlan[0].tile, &fixture.second);
    EXPECT_EQ(removalPlan[0].cacheKey, "test:1:1:0");
    EXPECT_EQ(removalPlan[1].tile, &fixture.first);
    EXPECT_EQ(removalPlan[1].cacheKey, "test:1:0:0");
    EXPECT_EQ(removalPlan[2].tile, &fixture.grandchild);
    EXPECT_EQ(removalPlan[2].cacheKey, "test:2:3:1");
}

TEST(TileSubtreeTraversal, ReverseRemovalPlanStartsWithDeepestDescendant) {
    SubtreeFixture fixture;

    const std::vector<TileSubtreeRemovalEntry> removalPlan =
        TileSubtreeTraversal::collectDescendantsForRemoval(
            fixture.root,
            simpleCacheKey);

    ASSERT_FALSE(removalPlan.empty());
    EXPECT_EQ(removalPlan.rbegin()->tile, &fixture.grandchild);
    for (const TileSubtreeRemovalEntry& entry : removalPlan) {
        EXPECT_NE(entry.tile, &fixture.root);
    }
}

TEST(TileSubtreeWorkTracker, ReportsInactiveForEmptyLifecycle) {
    TilesetTile root(TileKey{"test", 0, 0, 0}, Rectangle{});
    TileLoadLifecycle lifecycle;

    EXPECT_FALSE(TileSubtreeWorkTracker::hasActiveContentWork(
        root,
        lifecycle,
        TileCacheKey::forTile));
}

TEST(TileSubtreeWorkTracker, FindsPendingAndClaimedContentUploadWork) {
    TilesetTile root(TileKey{"test", 0, 0, 0}, Rectangle{});
    TilesetTile child(TileKey{"test", 1, 0, 0}, Rectangle{}, &root);
    root.children.push_back(&child);
    TileLoadLifecycle lifecycle;
    const std::string childCacheKey = TileCacheKey::forTile(child.key);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            child.key,
            childCacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileContentLoadResult::empty()});
    }
    EXPECT_TRUE(TileSubtreeWorkTracker::hasActiveContentWork(
        root,
        lifecycle,
        TileCacheKey::forTile));

    lifecycle.cancelAndEraseCacheKey(childCacheKey);

    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            child.key,
            childCacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileContentLoadResult::empty()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }
    EXPECT_TRUE(TileSubtreeWorkTracker::hasActiveContentWork(
        root,
        lifecycle,
        TileCacheKey::forTile));
}

TEST(TileSubtreeWorkTracker, FindsContentTerminalResultOnRoot) {
    TilesetTile root(TileKey{"test", 0, 0, 0}, Rectangle{});
    TileLoadLifecycle lifecycle;
    const std::string rootCacheKey = TileCacheKey::forTile(root.key);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addContentTerminalResult(
            PendingContentTerminalResult{
                root.key,
                rootCacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
    }

    EXPECT_TRUE(TileSubtreeWorkTracker::hasActiveContentWork(
        root,
        lifecycle,
        TileCacheKey::forTile));
}

TEST(TileSubtreeWorkTracker, FindsAndClearsTerrainLifecycleWork) {
    TilesetTile root(TileKey{"test", 0, 0, 0}, Rectangle{});
    TilesetTile child(TileKey{"test", 1, 0, 0}, Rectangle{}, &root);
    root.children.push_back(&child);
    TileLoadLifecycle lifecycle;
    const std::string childCacheKey = TileCacheKey::forTile(child.key);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().beginTerrainRequest(
            childCacheKey,
            CancellationToken{});
    }
    EXPECT_TRUE(TileSubtreeWorkTracker::hasActiveContentWork(
        root,
        lifecycle,
        TileCacheKey::forTile));

    lifecycle.cancelAndEraseCacheKey(childCacheKey);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainTerminalResult(
            PendingTerrainTerminalResult{
                child.key,
                childCacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
    }
    EXPECT_TRUE(TileSubtreeWorkTracker::hasActiveContentWork(
        root,
        lifecycle,
        TileCacheKey::forTile));

    lifecycle.cancelAndEraseCacheKey(childCacheKey);
    EXPECT_FALSE(TileSubtreeWorkTracker::hasActiveContentWork(
        root,
        lifecycle,
        TileCacheKey::forTile));
}
