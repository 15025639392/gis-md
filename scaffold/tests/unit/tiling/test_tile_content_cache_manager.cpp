#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileCacheOwnershipManager.h"
#include "earth_engine/tiling/TileContentCacheManager.h"
#include "earth_engine/tiling/TileLoadQueue.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace earth_engine;

namespace {

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
        tiles[rootCacheKey] = std::move(root);
        tiles[childCacheKey] = std::move(child);

        manager.updateTotalBytesUsed(tiles, lifecycle);
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

namespace {

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }
private:
    size_t byteSize_ = 0;
};

void makeGltfRenderReady(TilesetTile& tile) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    primitive.vertices[3].positionEcef = Vec3(1.0, 1.0, 0.0);
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.runtime.nodeIndex = 0;
    model->rasterOverlayDetails.setGeographicRectangle(tile.bounds);
    model->primitives.push_back(std::move(primitive));
    tile.content.renderContent.prepareGltfContent(
        std::move(model), Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    GltfPrimitiveRenderResources res;
    res.vertexBuffer = std::make_unique<DummyBuffer>(64);
    res.indexBuffer = std::make_unique<DummyBuffer>(12);
    res.indexCount = 6;
    res.vertexCount = 4;
    tile.content.renderContent.addGltfPrimitiveResource(std::move(res));
    tile.markRenderContentDone();
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
    makeGltfRenderReady(*tile);
    tiles[cacheKey] = std::move(tile);
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
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(cacheKey));
    EXPECT_EQ(tiles[cacheKey]->content.loadState, TileLoadState::Unloaded);
    EXPECT_EQ(tiles[cacheKey]->content.contentKind, TileContentKind::Unknown);
    EXPECT_FALSE(clearChildrenCalled);
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
    BatchesExternalChildrenClearUntilUnloadLoopCompletesLikeCesiumNative) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey rootAKey{"test", 0, 0, 0};
    const TileKey rootBKey{"test", 0, 1, 0};
    const std::string rootACacheKey = TileCacheKey::forTile(rootAKey);
    const std::string rootBCacheKey = TileCacheKey::forTile(rootBKey);

    auto rootA = std::make_unique<TilesetTile>(rootAKey, Rectangle{});
    auto rootB = std::make_unique<TilesetTile>(rootBKey, Rectangle{});
    rootA->content.loadState = TileLoadState::Done;
    rootA->content.contentKind = TileContentKind::External;
    rootB->content.loadState = TileLoadState::Done;
    rootB->content.contentKind = TileContentKind::External;
    tiles[rootACacheKey] = std::move(rootA);
    tiles[rootBCacheKey] = std::move(rootB);

    manager.updateTotalBytesUsed(tiles, lifecycle);
    manager.markEligibleForUnloading(tiles, rootACacheKey);
    manager.markEligibleForUnloading(tiles, rootBCacheKey);

    bool clearedA = false;
    bool clearedB = false;
    manager.unloadCachedBytes(
        0,
        0.0,
        false,
        tiles,
        lifecycle,
        nullptr,
        [&](TilesetTile& tile) {
            if (tile.key == rootAKey) {
                clearedA = true;
                EXPECT_FALSE(manager.unloadQueue().contains(rootBCacheKey));
            } else if (tile.key == rootBKey) {
                clearedB = true;
            }
        });

    EXPECT_TRUE(clearedA);
    EXPECT_TRUE(clearedB);
    EXPECT_FALSE(manager.unloadQueue().contains(rootACacheKey));
    EXPECT_FALSE(manager.unloadQueue().contains(rootBCacheKey));
    EXPECT_TRUE(tiles[rootACacheKey]->children.empty());
    EXPECT_TRUE(tiles[rootBCacheKey]->children.empty());
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

TEST(
    TileContentCacheManagerTest,
    ExternalSubtreeUnloadClearsDescendantIndexStateLikeCesiumNative) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    TileLoadQueue loadQueue;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    bool resourceSmoothingActive = false;
    int64_t maximumCachedBytes = 0;
    double unloadTimeLimitMs = 0.0;

    const TileKey rootKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const TileKey grandchildKey{"test", 2, 0, 0};
    const std::string rootCacheKey = TileCacheKey::forTile(rootKey);
    const std::string childCacheKey = TileCacheKey::forTile(childKey);
    const std::string grandchildCacheKey =
        TileCacheKey::forTile(grandchildKey);

    auto root = std::make_unique<TilesetTile>(rootKey, Rectangle{});
    auto child = std::make_unique<TilesetTile>(
        childKey,
        Rectangle{},
        root.get());
    auto grandchild = std::make_unique<TilesetTile>(
        grandchildKey,
        Rectangle{},
        child.get());
    TilesetTile* rootRaw = root.get();
    TilesetTile* childRaw = child.get();
    rootRaw->children.push_back(child.get());
    childRaw->children.push_back(grandchild.get());
    rootRaw->content.loadState = TileLoadState::Done;
    rootRaw->content.contentKind = TileContentKind::External;
    child->content.loadState = TileLoadState::Done;
    child->content.contentKind = TileContentKind::Render;
    grandchild->content.loadState = TileLoadState::Done;
    grandchild->content.contentKind = TileContentKind::Render;

    tiles[rootCacheKey] = std::move(root);
    tiles[childCacheKey] = std::move(child);
    tiles[grandchildCacheKey] = std::move(grandchild);
    lifecycle.emptyContentRegistry().insert(childCacheKey);
    lifecycle.emptyContentRegistry().insert(grandchildCacheKey);
    loadQueue.queue(childKey, TileLoadPriorityGroup::Normal, 1.0);
    loadQueue.queue(grandchildKey, TileLoadPriorityGroup::Normal, 2.0);
    manager.updateTotalBytesUsed(tiles, lifecycle);
    manager.markEligibleForUnloading(tiles, rootCacheKey);
    manager.markEligibleForUnloading(tiles, childCacheKey);
    manager.markEligibleForUnloading(tiles, grandchildCacheKey);
    ASSERT_TRUE(manager.unloadQueue().contains(rootCacheKey));
    ASSERT_TRUE(manager.unloadQueue().contains(childCacheKey));
    ASSERT_TRUE(manager.unloadQueue().contains(grandchildCacheKey));

    TileCacheOwnershipManager ownership(
        manager,
        lifecycle,
        loadQueue,
        tiles,
        resourceSmoothingActive,
        maximumCachedBytes,
        unloadTimeLimitMs);

    ownership.unloadCachedBytes(0, nullptr);

    EXPECT_EQ(tiles.end(), tiles.find(childCacheKey));
    EXPECT_EQ(tiles.end(), tiles.find(grandchildCacheKey));
    ASSERT_NE(tiles.end(), tiles.find(rootCacheKey));
    EXPECT_TRUE(rootRaw->children.empty());
    EXPECT_EQ(TileLoadState::Unloaded, rootRaw->content.loadState);
    EXPECT_EQ(TileContentKind::Unknown, rootRaw->content.contentKind);
    EXPECT_FALSE(manager.unloadQueue().contains(rootCacheKey));
    EXPECT_FALSE(manager.unloadQueue().contains(childCacheKey));
    EXPECT_FALSE(manager.unloadQueue().contains(grandchildCacheKey));
    EXPECT_TRUE(loadQueue.empty());
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(childCacheKey));
    EXPECT_FALSE(lifecycle.emptyContentRegistry().contains(
        grandchildCacheKey));
}

TEST(
    TileContentCacheManagerTest,
    ClearChildrenRecursivelyRemovesSubtreeIndexState) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    TileLoadQueue loadQueue;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    bool resourceSmoothingActive = false;
    int64_t maximumCachedBytes = 0;
    double unloadTimeLimitMs = 0.0;
    const TileKey rootKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const TileKey grandchildKey{"test", 2, 0, 0};
    const std::string rootCacheKey = TileCacheKey::forTile(rootKey);
    const std::string childCacheKey = TileCacheKey::forTile(childKey);
    const std::string grandchildCacheKey =
        TileCacheKey::forTile(grandchildKey);

    auto root = std::make_unique<TilesetTile>(rootKey, Rectangle{});
    auto child = std::make_unique<TilesetTile>(
        childKey,
        Rectangle{},
        root.get());
    auto grandchild = std::make_unique<TilesetTile>(
        grandchildKey,
        Rectangle{},
        child.get());
    TilesetTile* rootRaw = root.get();
    TilesetTile* childRaw = child.get();
    rootRaw->children.push_back(child.get());
    childRaw->children.push_back(grandchild.get());
    tiles[rootCacheKey] = std::move(root);
    tiles[childCacheKey] = std::move(child);
    tiles[grandchildCacheKey] = std::move(grandchild);

    TileCacheOwnershipManager ownership(
        manager,
        lifecycle,
        loadQueue,
        tiles,
        resourceSmoothingActive,
        maximumCachedBytes,
        unloadTimeLimitMs);

    ownership.clearChildrenRecursively(rootRaw, nullptr);

    EXPECT_TRUE(rootRaw->children.empty());
    EXPECT_EQ(tiles.end(), tiles.find(childCacheKey));
    EXPECT_EQ(tiles.end(), tiles.find(grandchildCacheKey));
}
