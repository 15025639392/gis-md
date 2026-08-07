#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/GpuUploadQueue.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileCacheOwnershipManager.h"
#include "earth_engine/tiling/TileContentCacheManager.h"
#include "earth_engine/tiling/TileContentResourceInvalidator.h"
#include "earth_engine/tiling/TileFillProxyPreparer.h"
#include "earth_engine/tiling/TileLoadQueue.h"
#include "earth_engine/tiling/TileRasterOverlayPrefetcher.h"
#include "earth_engine/tiling/TileScheme.h"

#include "../../helpers/MockRenderDevice.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace earth_engine;

namespace {

// markEligibleForUnloading now takes the tile the caller already holds; in
// tests the tile lives in the local registry map, so look it up (nullptr when
// absent, matching the old find-miss no-op).
const TilesetTile* tileForKey(
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
    const std::string& key) {
    auto it = tiles.find(key);
    return it == tiles.end() ? nullptr : it->second.get();
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
        tiles[rootCacheKey] = std::move(root);
        tiles[childCacheKey] = std::move(child);

        manager.updateTotalBytesUsed(tiles, lifecycle);
        manager.markEligibleForUnloading(tileForKey(tiles, rootCacheKey), rootCacheKey);
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
                        .takeHighestPriorityUpload(budget)
                        .has_value());
    }

    void unloadWithClear(bool& clearChildrenCalled) {
        manager.unloadCachedBytes(
            -1,
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

Rectangle projectForProvider(const TileScheme& scheme,
                             const Rectangle& geographicRectangle) {
    if (scheme.crsProfile() == "EPSG:3857") {
        return projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            geographicRectangle);
    }
    return geographicRectangle;
}

RasterOverlayDetails makeProviderDetails(const TileScheme& scheme,
                                         const Rectangle& geographicRectangle) {
    RasterOverlayDetails details;
    if (scheme.crsProfile() == "EPSG:3857") {
        details.rasterOverlayProjections = {
            RasterOverlayProjection::WebMercator};
        details.rasterOverlayRectangles = {
            projectForProvider(scheme, geographicRectangle)};
        details.boundingRegion = {geographicRectangle, 0.0, 0.0};
    } else {
        details.setGeographicRectangle(geographicRectangle);
    }
    return details;
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
    manager.markEligibleForUnloading(tileForKey(tiles, cacheKey), cacheKey);

    EXPECT_GT(manager.totalBytesUsed(), 0);
    EXPECT_TRUE(manager.unloadQueue().contains(cacheKey));

    bool clearChildrenCalled = false;
    manager.unloadCachedBytes(
        -1,
        0.0,
        false,
        tiles,
        lifecycle,
        nullptr,
        [&clearChildrenCalled](TilesetTile&) {
            clearChildrenCalled = true;
        });

    EXPECT_EQ(manager.totalBytesUsed(), 0);
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
    DoesNotUnloadZeroByteEligibleTileWithoutCachePressureLikeCesiumNative) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = TileCacheKey::forTile(key);
    auto tile = std::make_unique<TilesetTile>(key, Rectangle{});
    tile->content.loadState = TileLoadState::Done;
    tile->content.contentKind = TileContentKind::External;
    tiles[cacheKey] = std::move(tile);

    manager.updateTotalBytesUsed(tiles, lifecycle);
    manager.markEligibleForUnloading(tileForKey(tiles, cacheKey), cacheKey);
    manager.unloadCachedBytes(
        0,
        0.0,
        false,
        tiles,
        lifecycle,
        nullptr,
        [](TilesetTile&) {});

    EXPECT_EQ(0, manager.totalBytesUsed());
    EXPECT_TRUE(manager.unloadQueue().contains(cacheKey));
    EXPECT_EQ(TileLoadState::Done, tiles[cacheKey]->content.loadState);
    EXPECT_EQ(TileContentKind::External, tiles[cacheKey]->content.contentKind);
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

    manager.markEligibleForUnloading(tileForKey(tiles, cacheKey), cacheKey);
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
    manager.markEligibleForUnloading(tileForKey(tiles, cacheKey), cacheKey);

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
                        .takeHighestPriorityUpload(budget)
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
        -1,
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
        -1,
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
    manager.markEligibleForUnloading(tileForKey(tiles, rootACacheKey), rootACacheKey);
    manager.markEligibleForUnloading(tileForKey(tiles, rootBCacheKey), rootBCacheKey);

    bool clearedA = false;
    bool clearedB = false;
    manager.unloadCachedBytes(
        -1,
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
    std::vector<ActivatedRasterOverlay*> rasterOverlays;

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
    manager.markEligibleForUnloading(tileForKey(tiles, rootCacheKey), rootCacheKey);
    manager.markEligibleForUnloading(tileForKey(tiles, childCacheKey), childCacheKey);
    manager.markEligibleForUnloading(tileForKey(tiles, grandchildCacheKey), grandchildCacheKey);
    ASSERT_TRUE(manager.unloadQueue().contains(rootCacheKey));
    ASSERT_TRUE(manager.unloadQueue().contains(childCacheKey));
    ASSERT_TRUE(manager.unloadQueue().contains(grandchildCacheKey));

    TileCacheOwnershipManager ownership(
        manager,
        lifecycle,
        loadQueue,
        tiles,
        rasterOverlays,
        resourceSmoothingActive,
        maximumCachedBytes,
        unloadTimeLimitMs);

    ownership.unloadCachedBytes(-1, nullptr);

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
    std::vector<ActivatedRasterOverlay*> rasterOverlays;
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
        rasterOverlays,
        resourceSmoothingActive,
        maximumCachedBytes,
        unloadTimeLimitMs);

    ownership.clearChildrenRecursively(rootRaw, nullptr);

    EXPECT_TRUE(rootRaw->children.empty());
    EXPECT_EQ(tiles.end(), tiles.find(childCacheKey));
    EXPECT_EQ(tiles.end(), tiles.find(grandchildCacheKey));
}

TEST(
    TileContentCacheManagerTest,
    TerrainLedgerExcludesProviderOwnedSharedRasterTextures) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activated(*overlay);
    RasterOverlayTileProvider* provider = activated.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);

    const TileKey parentKey{overlay->getTileScheme().id(), 2, 1, 1};
    const TileKey childKey{overlay->getTileScheme().id(), 3, 2, 2};
    const Rectangle parentBounds =
        overlay->getTileScheme().tileToRectangle(parentKey);
    const Rectangle childBounds =
        overlay->getTileScheme().tileToRectangle(childKey);
    const RasterOverlayDetails parentDetails =
        makeProviderDetails(overlay->getTileScheme(), parentBounds);
    const RasterOverlayDetails childDetails =
        makeProviderDetails(overlay->getTileScheme(), childBounds);
    std::vector<RasterOverlayProjection> missing;

    auto parentTile = std::make_unique<TilesetTile>(parentKey, parentBounds);
    RasterMappedToTilesetTile& parentMapping =
        parentTile->rasterOverlayState.ensureMapping(0);
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        nullptr,
        0);
    RasterOverlayTile* parentRaster = parentMapping.getLoadingTile();
    ASSERT_NE(nullptr, parentRaster);
    parentRaster->setTexture(std::make_unique<earth_engine::testing::DummyTexture>(
        4,
        4));
    parentMapping.update(
        parentKey,
        parentDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        nullptr,
        0);
    ASSERT_EQ(parentRaster, parentMapping.getReadyTile());

    auto childTile =
        std::make_unique<TilesetTile>(childKey, childBounds, parentTile.get());
    childTile->geometricError = 100.0;
    RasterMappedToTilesetTile& childMapping =
        childTile->rasterOverlayState.ensureMapping(0);
    childMapping.update(
        childKey,
        childDetails,
        512.0,
        512.0,
        *provider,
        nullptr,
        missing,
        parentTile.get(),
        0);

    FrameResourceBudgetConfig config;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    std::vector<ActivatedRasterOverlay*> overlays{&activated};
    TileRasterOverlayPrefetcher::prefetch(
        *childTile,
        overlays,
        {0},
        nullptr,
        16.0,
        budget);

    ASSERT_EQ(parentRaster, childMapping.getReadyTile());
    ASSERT_EQ(RasterMappedToTilesetTile::ReadyTileSource::Ancestor,
              childMapping.getReadyTileSource());

    const std::string parentCacheKey = TileCacheKey::forTile(parentKey);
    const std::string childCacheKey = TileCacheKey::forTile(childKey);
    tiles.emplace(parentCacheKey, std::move(parentTile));
    tiles.emplace(childCacheKey, std::move(childTile));

    manager.updateTotalBytesUsed(tiles, lifecycle);
    ASSERT_EQ(0, manager.totalBytesUsed());
    ASSERT_EQ(4 * 4 * 4, provider->tileTextureBytesUsed());
    manager.markEligibleForUnloading(
        tileForKey(tiles, childCacheKey),
        childCacheKey);

    manager.unloadCachedBytes(
        0,
        0.0,
        true,
        tiles,
        lifecycle,
        nullptr,
        [](TilesetTile&) {});

    EXPECT_EQ(0, manager.totalBytesUsed());
    EXPECT_EQ(4 * 4 * 4, provider->tileTextureBytesUsed());
    EXPECT_FALSE(manager.unloadQueue().contains(childCacheKey));
    EXPECT_EQ(TileLoadState::Unloaded, tiles[childCacheKey]->content.loadState);
}

TEST(
    TileContentCacheManagerTest,
    ReconcilesOnlyTheChangedTileAndSubtractsExactAccountedBytes) {
    TileContentCacheManager manager;
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});

    makeGltfRenderReady(tile);
    ASSERT_TRUE(manager.reconcileTileBytes(tile));
    const int64_t loadedBytes = manager.totalBytesUsed();
    ASSERT_GT(loadedBytes, 0);
    EXPECT_EQ(
        loadedBytes,
        manager.accountedTileBytes(TileCacheKey::forTile(tile.key)));

    tile.content.renderContent.clearRenderContent();
    EXPECT_TRUE(manager.reconcileTileBytes(tile));
    EXPECT_EQ(0, manager.totalBytesUsed());
    EXPECT_EQ(
        0,
        manager.accountedTileBytes(TileCacheKey::forTile(tile.key)));

    EXPECT_FALSE(manager.reconcileTileBytes(tile));
}

TEST(
    TileContentCacheManagerTest,
    ResourceRevisionReconcilesSameSizeOwnershipReplacementWithoutScanning) {
    TileContentCacheManager manager;
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});

    GltfPrimitiveRenderResources first;
    first.vertexBuffer = std::make_unique<DummyBuffer>(64);
    tile.content.renderContent.addGltfPrimitiveResource(std::move(first));
    ASSERT_TRUE(manager.reconcileTileBytes(tile));
    ASSERT_EQ(64, manager.totalBytesUsed());
    EXPECT_FALSE(manager.reconcileTileBytes(tile));

    tile.content.renderContent.clearGltfPrimitiveResources();
    GltfPrimitiveRenderResources replacement;
    replacement.vertexBuffer = std::make_unique<DummyBuffer>(64);
    tile.content.renderContent.addGltfPrimitiveResource(
        std::move(replacement));

    EXPECT_TRUE(manager.reconcileTileBytes(tile));
    EXPECT_EQ(64, manager.totalBytesUsed());
    EXPECT_FALSE(manager.reconcileTileBytes(tile));
}

TEST(
    TileContentCacheManagerTest,
    ScopedGltfEditAdvancesRevisionForRetainedByteChanges) {
    TileContentCacheManager manager;
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});

    makeGltfRenderReady(tile);
    ASSERT_TRUE(manager.reconcileTileBytes(tile));
    const int64_t bytesBeforeEdit = manager.totalBytesUsed();

    {
        auto model = tile.content.renderContent.editGltfContent();
        ASSERT_NE(nullptr, model);
        ASSERT_FALSE(model->primitives.empty());
        model->primitives.front().vertices.emplace_back();
    }

    EXPECT_TRUE(manager.reconcileTileBytes(tile));
    EXPECT_EQ(
        bytesBeforeEdit + static_cast<int64_t>(sizeof(SurfaceVertex)),
        manager.totalBytesUsed());
    EXPECT_FALSE(manager.reconcileTileBytes(tile));
}

TEST(
    TileContentCacheManagerTest,
    OwnershipTotalIncludesPendingGpuPayloadAndCancellationRemovesIt) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    TileLoadQueue loadQueue;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    std::vector<ActivatedRasterOverlay*> rasterOverlays;
    bool resourceSmoothingActive = false;
    int64_t maximumCachedBytes = 1024;
    double unloadTimeLimitMs = 0.0;
    GpuUploadQueue gpuUploadQueue;

    GpuReadyPrimitive primitive;
    primitive.vertexBytes.resize(32);
    primitive.indexBytes.resize(4 * sizeof(uint32_t));
    primitive.indexCount = 4;
    GpuReadyData data;
    data.primitives.push_back(std::move(primitive));
    const int64_t pendingBytes = data.byteSize();
    gpuUploadQueue.push(PendingGpuUpload{
        TileKey{"test", 0, 0, 0},
        "pending",
        std::move(data)});

    TileCacheOwnershipManager ownership(
        manager,
        lifecycle,
        loadQueue,
        tiles,
        rasterOverlays,
        resourceSmoothingActive,
        maximumCachedBytes,
        unloadTimeLimitMs,
        &gpuUploadQueue);

    EXPECT_EQ(pendingBytes, ownership.totalBytesUsed());
    ownership.eraseTileIndexState("pending");
    EXPECT_EQ(0, gpuUploadQueue.pendingBytes());
    EXPECT_EQ(0, ownership.totalBytesUsed());
}

TEST(
    TileContentCacheManagerTest,
    AsyncPendingTransitionRemovesStaleUnloadQueueEntry) {
    TileContentCacheManager manager;
    uint64_t resourceRevision = 1;
    TileContentResourceInvalidator invalidator(
        resourceRevision,
        manager);
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.loadState = TileLoadState::Done;
    tile.content.contentKind = TileContentKind::Render;

    invalidator.markTileResourcesChanged(tile);
    const std::string cacheKey = TileCacheKey::forTile(tile.key);
    ASSERT_TRUE(manager.unloadQueue().contains(cacheKey));

    tile.content.renderContent.asyncGpuUploadPending = true;
    invalidator.markTileResourcesChanged(tile);

    EXPECT_FALSE(manager.unloadQueue().contains(cacheKey));
}

TEST(
    TileContentCacheManagerTest,
    ReconcileOnlyUpdatesFillLedgerWithoutInvalidatingSelection) {
    TileContentCacheManager manager;
    uint64_t resourceRevision = 17;
    TileContentResourceInvalidator invalidator(
        resourceRevision,
        manager);
    TilesetTile tile(
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, 20.0, -2.0, 28.0));
    earth_engine::testing::MockRenderDevice device;

    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        tile,
        &device,
        1));
    invalidator.reconcileTileResources(tile);

    EXPECT_EQ(17u, resourceRevision);
    EXPECT_GT(
        manager.accountedTileBytes(TileCacheKey::forTile(tile.key)),
        0);
    const std::string cacheKey = TileCacheKey::forTile(tile.key);
    EXPECT_TRUE(manager.unloadQueue().contains(cacheKey));

    invalidator.markTileResourcesChanged(tile);
    EXPECT_EQ(18u, resourceRevision);

    TileContentLifecycleManager lifecycle;
    EXPECT_EQ(
        TileCacheUnloadContentResult::Remove,
        manager.unloadTileContent(tile, lifecycle, nullptr));
    EXPECT_FALSE(tile.content.renderContent.isFillReady());
    EXPECT_EQ(0, manager.accountedTileBytes(cacheKey));
}

TEST(
    TileContentCacheManagerTest,
    UnloadedFillIsEvictedThroughLruAndItsLedgerReturnsToZero) {
    TileContentCacheManager manager;
    TileContentLifecycleManager lifecycle;
    uint64_t resourceRevision = 31;
    TileContentResourceInvalidator invalidator(
        resourceRevision,
        manager);
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    const TileKey key{"Geographic-TMS", 2, 1, 1};
    const std::string cacheKey = TileCacheKey::forTile(key);
    auto tile = std::make_unique<TilesetTile>(
        key,
        Rectangle::fromDegrees(-10.0, 20.0, -2.0, 28.0));
    TilesetTile* tilePtr = tile.get();
    tiles.emplace(cacheKey, std::move(tile));
    earth_engine::testing::MockRenderDevice device;

    ASSERT_TRUE(TileFillProxyPreparer::ensureFillProxy(
        *tilePtr,
        &device,
        1));
    tilePtr->rasterOverlayState.ensureMapping(0);
    invalidator.reconcileTileResources(*tilePtr);
    ASSERT_GT(manager.totalBytesUsed(), 0);
    ASSERT_TRUE(manager.unloadQueue().contains(cacheKey));

    manager.unloadCachedBytes(
        0,
        0.0,
        false,
        tiles,
        lifecycle,
        nullptr,
        [](TilesetTile&) {});

    EXPECT_EQ(31u, resourceRevision);
    EXPECT_EQ(0, manager.totalBytesUsed());
    EXPECT_EQ(0, manager.accountedTileBytes(cacheKey));
    EXPECT_FALSE(manager.unloadQueue().contains(cacheKey));
    EXPECT_EQ(TileLoadState::Unloaded, tilePtr->content.loadState);
    EXPECT_EQ(TileContentKind::Unknown, tilePtr->content.contentKind);
    EXPECT_FALSE(tilePtr->content.renderContent.hasFillModel());
    EXPECT_FALSE(tilePtr->content.renderContent.hasFillResources());
    EXPECT_FALSE(tilePtr->content.renderContent.isFillReady());
    EXPECT_EQ(0u, tilePtr->rasterOverlayState.mappingCount());
}
