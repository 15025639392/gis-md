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

struct ExternalSubtreeFixture {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    TileKey rootKey{"test", 0, 0, 0};
    TileKey childKey{"test", 1, 0, 0};
    std::string rootCacheKey = TileCacheKey::forTile(rootKey);
    std::string childCacheKey = TileCacheKey::forTile(childKey);
    TilesetTile* rootRaw = nullptr;

    ExternalSubtreeFixture() {
        auto root = std::make_unique<TilesetTile>(rootKey, Rectangle{});
        auto child = std::make_unique<TilesetTile>(
            childKey,
            Rectangle{},
            root.get());
        rootRaw = root.get();
        rootRaw->children.push_back(child.get());
        rootRaw->content.loadState = TileLoadState::Done;
        rootRaw->content.contentKind = TileContentKind::External;
        lifecycle.heightmapTerrainCache()[rootCacheKey] = makeFlatHeightmap(4.0f);
        tiles[rootCacheKey] = std::move(root);
        tiles[childCacheKey] = std::move(child);

        manager.updateTotalBytesUsed(
            tiles,
            lifecycle,
            true);
        manager.markEligibleForUnloading(tiles, rootCacheKey);
    }

    void addChildUploadWork() {
        std::lock_guard<std::mutex> lock(lifecycle.loadLifecycle().mutex());
        lifecycle.loadLifecycle().pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
                childKey,
                childCacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
    }

    void claimChildUploadWork() {
        FrameResourceBudgetConfig config;
        config.maxMainThreadFinalizesPerFrame = 1;
        FrameResourceBudget budget;
        budget.beginFrame(1, config);
        std::lock_guard<std::mutex> lock(lifecycle.loadLifecycle().mutex());
        lifecycle.loadLifecycle().pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
                childKey,
                childCacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
        ASSERT_TRUE(lifecycle.loadLifecycle()
                        .pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    void unloadWithClear(bool& clearChildrenCalled) {
        manager.unloadCachedBytes(
            0,
            0.0,
            false,
            true,
            tiles,
            lifecycle,
            nullptr,
            [this, &clearChildrenCalled](TilesetTile& tile) {
                clearChildrenCalled = true;
                tile.children.clear();
                tiles.erase(childCacheKey);
            });
    }
};

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
    lifecycle.heightmapTerrainCache()[cacheKey] = makeFlatHeightmap(4.0f);
    lifecycle.emptyContentRegistry().insert(cacheKey);

    manager.updateTotalBytesUsed(
        tiles,
        lifecycle,
        true);
    manager.markEligibleForUnloading(tiles, cacheKey);

    EXPECT_GT(manager.totalBytesUsed(), 0);
    EXPECT_TRUE(manager.unloadQueue().contains(cacheKey));

    bool clearChildrenCalled = false;
    manager.unloadCachedBytes(
        0,
        0.0,
        false,
        true,
        tiles,
        lifecycle,
        nullptr,
        [&clearChildrenCalled](TilesetTile&) {
            clearChildrenCalled = true;
        });

    EXPECT_EQ(manager.totalBytesUsed(), 0);
    EXPECT_FALSE(manager.cacheBytesDirty());
    EXPECT_FALSE(manager.unloadQueue().contains(cacheKey));
    EXPECT_EQ(lifecycle.heightmapTerrainCache().find(cacheKey),
              lifecycle.heightmapTerrainCache().end());
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(cacheKey));
    EXPECT_EQ(tiles[cacheKey]->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(tiles[cacheKey]->content.contentKind, TileContentKind::Unknown);
    EXPECT_FALSE(clearChildrenCalled);
}

TEST(
    TileContentCacheManagerTest,
    RenderContentUnloadErasesRetainedHeightmapTerrainCache) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey key{"Geographic-TMS", 1, 0, 0};
    const std::string cacheKey = TileCacheKey::forTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.loadState = TileLoadState::Done;
    tile->content.contentKind = TileContentKind::Render;
    TilesetTile* tileRaw = tile.get();
    tiles[cacheKey] = std::move(tile);
    lifecycle.heightmapTerrainCache()[cacheKey] = makeFlatHeightmap(7.0f);
    lifecycle.emptyContentRegistry().insert(cacheKey);

    manager.markEligibleForUnloading(tiles, cacheKey);

    const TileCacheUnloadContentResult result =
        manager.unloadTileContent(
            *tileRaw,
            lifecycle,
            nullptr);

    EXPECT_EQ(TileCacheUnloadContentResult::Remove, result);
    EXPECT_EQ(lifecycle.heightmapTerrainCache().find(cacheKey),
              lifecycle.heightmapTerrainCache().end());
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(cacheKey));
    EXPECT_EQ(TileLoadState::Unloaded, tileRaw->content.loadState);
    EXPECT_EQ(TileContentKind::Unknown, tileRaw->content.contentKind);
}

TEST(
    TileContentCacheManagerTest,
    RenderContentUnloadClearsRasterOverlayMissingProjectionsLikeCesiumNative) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;

    const TileKey key{"test", 0, 0, 0};
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.loadState = TileLoadState::Done;
    tile->content.contentKind = TileContentKind::Render;
    tile->rasterOverlayState.ensureMappingSlots(1);
    tile->rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection::WebMercator);
    TilesetTile* tileRaw = tile.get();

    const TileCacheUnloadContentResult result =
        manager.unloadTileContent(
            *tile,
            lifecycle,
            nullptr);

    EXPECT_EQ(TileCacheUnloadContentResult::Remove, result);
    EXPECT_EQ(TileLoadState::Unloaded, tileRaw->content.loadState);
    EXPECT_EQ(TileContentKind::Unknown, tileRaw->content.contentKind);
    EXPECT_TRUE(tileRaw->rasterOverlayState.mappings().empty());
    EXPECT_TRUE(tileRaw->rasterOverlayState.missingProjections().empty());
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
        true,
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
    lifecycle.heightmapTerrainCache()[cacheKey] = makeFlatHeightmap(2.0f);
    lifecycle.emptyContentRegistry().insert(cacheKey);
    loadQueue.queue(key, TileLoadPriorityGroup::Normal, 0.0);
    manager.markEligibleForUnloading(tiles, cacheKey);

    {
        FrameResourceBudgetConfig config;
        config.maxMainThreadFinalizesPerFrame = 1;
        FrameResourceBudget budget;
        budget.beginFrame(1, config);
        std::lock_guard<std::mutex> lock(lifecycle.loadLifecycle().mutex());
        lifecycle.loadLifecycle().pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
        ASSERT_TRUE(lifecycle.loadLifecycle()
                        .pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    manager.eraseTileIndexState(cacheKey, lifecycle, loadQueue);

    EXPECT_FALSE(manager.unloadQueue().contains(cacheKey));
    EXPECT_EQ(lifecycle.heightmapTerrainCache().find(cacheKey),
              lifecycle.heightmapTerrainCache().end());
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(cacheKey));
    EXPECT_TRUE(loadQueue.empty());
    EXPECT_FALSE(lifecycle.loadLifecycle().containsWorkForCacheKey(cacheKey));
    EXPECT_FALSE(lifecycle.loadLifecycle().hasPendingWork());
}

TEST(
    TileContentCacheManagerTest,
    EraseIndexStateErasesRetainedHeightmapTerrainCache) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    TileLoadQueue loadQueue;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey key{"Geographic-TMS", 1, 0, 0};
    const std::string cacheKey = TileCacheKey::forTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.loadState = TileLoadState::Done;
    tile->content.contentKind = TileContentKind::Render;
    tiles[cacheKey] = std::move(tile);
    lifecycle.heightmapTerrainCache()[cacheKey] = makeFlatHeightmap(9.0f);
    lifecycle.emptyContentRegistry().insert(cacheKey);
    loadQueue.queue(key, TileLoadPriorityGroup::Normal, 0.0);
    manager.markEligibleForUnloading(tiles, cacheKey);

    {
        FrameResourceBudgetConfig config;
        config.maxMainThreadFinalizesPerFrame = 1;
        FrameResourceBudget budget;
        budget.beginFrame(1, config);
        std::lock_guard<std::mutex> lock(lifecycle.loadLifecycle().mutex());
        lifecycle.loadLifecycle().pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
        ASSERT_TRUE(lifecycle.loadLifecycle()
                        .pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    manager.eraseTileIndexState(
        cacheKey,
        lifecycle,
        loadQueue);

    EXPECT_FALSE(manager.unloadQueue().contains(cacheKey));
    EXPECT_EQ(lifecycle.heightmapTerrainCache().find(cacheKey),
              lifecycle.heightmapTerrainCache().end());
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(cacheKey));
    EXPECT_TRUE(loadQueue.empty());
    EXPECT_FALSE(lifecycle.loadLifecycle().containsWorkForCacheKey(cacheKey));
    EXPECT_FALSE(lifecycle.loadLifecycle().hasPendingWork());
}

TEST(
    TileContentCacheManagerTest,
    DefersExternalSubtreeUnloadWhileChildUploadIsPending) {
    ExternalSubtreeFixture fixture;
    fixture.addChildUploadWork();
    bool clearChildrenCalled = false;

    fixture.manager.unloadCachedBytes(
        0,
        0.0,
        false,
        true,
        fixture.tiles,
        fixture.lifecycle,
        nullptr,
        [&clearChildrenCalled](TilesetTile&) {
            clearChildrenCalled = true;
        });

    EXPECT_TRUE(fixture.manager.unloadQueue().contains(
        fixture.rootCacheKey));
    EXPECT_EQ(fixture.tiles[fixture.rootCacheKey]->content.contentKind,
              TileContentKind::External);
    EXPECT_EQ(fixture.tiles[fixture.rootCacheKey]->content.loadState,
              TileLoadState::Done);
    EXPECT_TRUE(fixture.lifecycle.loadLifecycle().containsWorkForCacheKey(
        fixture.childCacheKey));
    EXPECT_FALSE(clearChildrenCalled);
}

TEST(
    TileContentCacheManagerTest,
    DefersExternalSubtreeUnloadWhileChildUploadIsClaimed) {
    ExternalSubtreeFixture fixture;
    fixture.claimChildUploadWork();
    bool clearChildrenCalled = false;

    fixture.manager.unloadCachedBytes(
        0,
        0.0,
        false,
        true,
        fixture.tiles,
        fixture.lifecycle,
        nullptr,
        [&clearChildrenCalled](TilesetTile&) {
            clearChildrenCalled = true;
        });

    EXPECT_TRUE(fixture.manager.unloadQueue().contains(
        fixture.rootCacheKey));
    EXPECT_EQ(fixture.tiles[fixture.rootCacheKey]->content.contentKind,
              TileContentKind::External);
    EXPECT_EQ(fixture.tiles[fixture.rootCacheKey]->content.loadState,
              TileLoadState::Done);
    EXPECT_TRUE(fixture.lifecycle.loadLifecycle().containsWorkForCacheKey(
        fixture.childCacheKey));
    EXPECT_TRUE(fixture.lifecycle.loadLifecycle().hasPendingWork());
    EXPECT_FALSE(clearChildrenCalled);
}

TEST(
    TileContentCacheManagerTest,
    RetriesExternalSubtreeUnloadAfterClaimedUploadCompletes) {
    ExternalSubtreeFixture fixture;
    fixture.claimChildUploadWork();
    bool clearChildrenCalled = false;

    fixture.unloadWithClear(clearChildrenCalled);
    ASSERT_TRUE(fixture.manager.unloadQueue().contains(
        fixture.rootCacheKey));
    ASSERT_FALSE(clearChildrenCalled);
    ASSERT_FALSE(fixture.rootRaw->children.empty());

    fixture.lifecycle.loadLifecycle().cancelAndEraseCacheKey(
        fixture.childCacheKey);
    fixture.unloadWithClear(clearChildrenCalled);

    EXPECT_FALSE(fixture.manager.unloadQueue().contains(
        fixture.rootCacheKey));
    EXPECT_TRUE(clearChildrenCalled);
    EXPECT_TRUE(fixture.rootRaw->children.empty());
    EXPECT_EQ(fixture.tiles.find(fixture.childCacheKey), fixture.tiles.end());
    EXPECT_FALSE(fixture.lifecycle.loadLifecycle().containsWorkForCacheKey(
        fixture.childCacheKey));
    EXPECT_EQ(fixture.rootRaw->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(fixture.rootRaw->content.contentKind, TileContentKind::Unknown);
}

TEST(
    TileContentCacheManagerTest,
    RetriesExternalSubtreeUnloadAfterPendingWorkCompletes) {
    ExternalSubtreeFixture fixture;
    fixture.tiles[fixture.childCacheKey]->content.loadState =
        TileLoadState::Done;
    fixture.tiles[fixture.childCacheKey]->content.contentKind =
        TileContentKind::Render;
    fixture.addChildUploadWork();
    bool clearChildrenCalled = false;

    fixture.unloadWithClear(clearChildrenCalled);
    ASSERT_TRUE(fixture.manager.unloadQueue().contains(
        fixture.rootCacheKey));
    ASSERT_FALSE(clearChildrenCalled);
    ASSERT_NE(fixture.tiles.find(fixture.childCacheKey), fixture.tiles.end());

    fixture.lifecycle.loadLifecycle().cancelAndEraseCacheKey(
        fixture.childCacheKey);
    fixture.unloadWithClear(clearChildrenCalled);

    EXPECT_FALSE(fixture.manager.unloadQueue().contains(
        fixture.rootCacheKey));
    EXPECT_TRUE(clearChildrenCalled);
    EXPECT_TRUE(fixture.rootRaw->children.empty());
    EXPECT_EQ(fixture.tiles.find(fixture.childCacheKey), fixture.tiles.end());
    EXPECT_EQ(fixture.rootRaw->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(fixture.rootRaw->content.contentKind, TileContentKind::Unknown);
}
