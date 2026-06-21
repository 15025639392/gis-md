#include <gtest/gtest.h>

#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileContentCacheManager.h"
#include "earth_engine/tiling/TileLoadQueue.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights.assign(4, heightMeters);
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

} // namespace

TEST(
    TileContentCacheManagerTest,
    OwnsByteAccountingUnloadQueueAndRenderContentUnload) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = TileCacheKey::forTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.loadState = TileLoadState::Done;
    tile->content.contentKind = TileContentKind::Render;
    tiles[cacheKey] = std::move(tile);
    lifecycle.terrainCache()[cacheKey] = makeFlatHeightmap(4.0f);
    lifecycle.emptyContentRegistry().insert(cacheKey);

    manager.updateTotalBytesUsed(tiles, lifecycle);
    manager.markEligibleForUnloading(tiles, cacheKey);

    EXPECT_GT(manager.totalBytesUsed(), 0);
    EXPECT_TRUE(manager.unloadQueue().contains(cacheKey));

    bool clearChildrenCalled = false;
    manager.unloadCachedBytes(
        0,
        0.0,
        false,
        tiles,
        lifecycle,
        nullptr,
        [&clearChildrenCalled](TilesetTile&) {
            clearChildrenCalled = true;
        });

    EXPECT_EQ(manager.totalBytesUsed(), 0);
    EXPECT_FALSE(manager.cacheBytesDirty());
    EXPECT_FALSE(manager.unloadQueue().contains(cacheKey));
    EXPECT_EQ(lifecycle.terrainCache().find(cacheKey),
              lifecycle.terrainCache().end());
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(cacheKey));
    EXPECT_EQ(tiles[cacheKey]->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(tiles[cacheKey]->content.contentKind, TileContentKind::Unknown);
    EXPECT_FALSE(clearChildrenCalled);
}

TEST(
    TileContentCacheManagerTest,
    ClearsStaleEmptyMarkerWhenUnknownContentUnloads) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = TileCacheKey::forTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.loadState = TileLoadState::Failed;
    tile->content.contentKind = TileContentKind::Unknown;
    tiles[cacheKey] = std::move(tile);
    lifecycle.emptyContentRegistry().insert(cacheKey);

    manager.markEligibleForUnloading(tiles, cacheKey);
    manager.unloadCachedBytes(
        -1,
        0.0,
        false,
        tiles,
        lifecycle,
        nullptr,
        [](TilesetTile&) {});

    EXPECT_FALSE(manager.unloadQueue().contains(cacheKey));
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(cacheKey));
    EXPECT_EQ(tiles[cacheKey]->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(tiles[cacheKey]->content.contentKind, TileContentKind::Unknown);
}

TEST(
    TileContentCacheManagerTest,
    EraseIndexStateClearsClaimedUploadWork) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    TileLoadQueue loadQueue;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = TileCacheKey::forTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.loadState = TileLoadState::Done;
    tile->content.contentKind = TileContentKind::Render;
    tiles[cacheKey] = std::move(tile);
    lifecycle.terrainCache()[cacheKey] = makeFlatHeightmap(2.0f);
    lifecycle.emptyContentRegistry().insert(cacheKey);
    loadQueue.queue(key, TileLoadPriorityGroup::Normal, 0.0);
    manager.markEligibleForUnloading(tiles, cacheKey);

    {
        FrameResourceBudgetConfig config;
        config.maxMainThreadFinalizesPerFrame = 1;
        FrameResourceBudget budget;
        budget.beginFrame(1, config);
        std::lock_guard<std::mutex> lock(lifecycle.loadLifecycle().mutex());
        lifecycle.loadLifecycle().pendingLoads().addContentUpload(
            PendingContentUpload{
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
                TileContentLoadResult::empty()});
        ASSERT_TRUE(lifecycle.loadLifecycle()
                        .pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    manager.eraseTileIndexState(cacheKey, lifecycle, loadQueue);

    EXPECT_FALSE(manager.unloadQueue().contains(cacheKey));
    EXPECT_EQ(lifecycle.terrainCache().find(cacheKey),
              lifecycle.terrainCache().end());
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(cacheKey));
    EXPECT_TRUE(loadQueue.empty());
    EXPECT_FALSE(lifecycle.loadLifecycle().containsWorkForCacheKey(cacheKey));
    EXPECT_FALSE(lifecycle.loadLifecycle().hasPendingWork());
}
