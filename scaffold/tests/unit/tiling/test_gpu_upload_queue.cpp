#include <gtest/gtest.h>

#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTile.h"
#include "earth_engine/renderer/IPrepareRendererResources.h"
#include "earth_engine/tiling/GpuUploadQueue.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileEmptyContentRegistry.h"
#include "earth_engine/tiling/TileLoadLifecycle.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/TilesetContentLifecycleCoordinator.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace earth_engine;

namespace {

class DummyTexture final : public Texture {
public:
    DummyTexture(int width, int height) : width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }
    size_t sizeBytes() const override {
        return static_cast<size_t>(width_) *
               static_cast<size_t>(height_) * 4u;
    }

private:
    int width_ = 0;
    int height_ = 0;
};

class RecordingPrepareRendererResources final
    : public IPrepareRendererResources {
public:
    void attachRasterInMainThread(
        const TileKey&,
        int32_t,
        std::shared_ptr<const RasterOverlayTile>,
        Texture*,
        float,
        float,
        float,
        float) override {
        ++attachCount;
    }

    void detachRasterInMainThread(
        const TileKey&,
        int32_t) noexcept override {
        ++detachCount;
    }

    int attachCount = 0;
    int detachCount = 0;
};

PendingGpuUpload makeUpload(const std::string& cacheKey) {
    GpuReadyPrimitive primitive;
    primitive.vertexBytes.resize(8);
    primitive.indexBytes.resize(2 * sizeof(uint32_t));
    primitive.indexCount = 2;
    primitive.instances = GpuReadyPrimitive::InstanceData{};
    primitive.instances->bytes.resize(5);

    GpuReadyData data;
    data.primitives.push_back(std::move(primitive));
    GpuReadyData::TextureData texture;
    texture.pixels.resize(7);
    data.textures.push_back(std::move(texture));

    return PendingGpuUpload{
        TileKey{"test", 0, 0, 0},
        cacheKey,
        std::move(data)};
}

} // namespace

TEST(GpuUploadQueueTest, TracksPendingBytesAcrossPushPopEraseAndClear) {
    GpuUploadQueue queue;
    queue.push(makeUpload("a"));
    queue.push(makeUpload("b"));

    constexpr int64_t kUploadBytes = 8 + 2 * sizeof(uint32_t) + 5 + 7;
    EXPECT_EQ(2u, queue.size());
    EXPECT_EQ(2 * kUploadBytes, queue.pendingBytes());

    EXPECT_EQ(1u, queue.eraseCacheKey("a"));
    EXPECT_EQ(1u, queue.size());
    EXPECT_EQ(kUploadBytes, queue.pendingBytes());
    EXPECT_EQ(0u, queue.eraseCacheKey("missing"));

    std::optional<PendingGpuUpload> popped = queue.tryPop();
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ("b", popped->cacheKey);
    EXPECT_EQ(0u, queue.size());
    EXPECT_EQ(0, queue.pendingBytes());

    queue.push(makeUpload("c"));
    queue.clear();
    EXPECT_FALSE(queue.hasWork());
    EXPECT_EQ(0, queue.pendingBytes());
}

TEST(GpuUploadQueueTest, InvalidPayloadReleasesLifecycleUploadClaim) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "invalid-gpu-payload";
    const Rectangle initialBounds =
        Rectangle::fromDegrees(-20.0, -10.0, 20.0, 10.0);
    const Rectangle updatedBounds =
        Rectangle::fromDegrees(-15.0, -5.0, 15.0, 5.0);
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig budgetConfig;
    budgetConfig.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, budgetConfig);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{
            TileLoadDomain::TerrainContent,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableGltfTerrain(
                std::make_unique<GltfModel>())});
        ASSERT_TRUE(
            lifecycle.pendingLoads()
                .takeHighestPriorityUpload(budget)
                .has_value());
    }
    ASSERT_TRUE(lifecycle.containsWorkForCacheKey(cacheKey));

    TilesetTile tile(key, initialBounds);
    tile.setBoundingVolume(
        TileBoundingVolume::fromRegion(initialBounds, -100.0, 100.0));
    tile.initialBoundingVolume = tile.boundingVolume;
    tile.setContentBoundingVolume(
        TileBoundingVolume::fromRegion(initialBounds, -50.0, 50.0));
    tile.initialContentBoundingVolume = tile.contentBoundingVolume;
    tile.setBoundingVolume(
        TileBoundingVolume::fromRegion(updatedBounds, -200.0, 200.0));
    tile.setContentBoundingVolume(
        TileBoundingVolume::fromRegion(updatedBounds, -150.0, 150.0));
    tile.content.renderContent.prepareGltfContent(
        std::make_unique<GltfModel>(),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.markRenderContentLoaded();
    tile.content.renderContent.asyncGpuUploadPending = true;

    RasterOverlay overlay(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activeOverlay(overlay);
    RasterOverlayTileProvider* provider =
        activeOverlay.ensureTileProvider(nullptr);
    ASSERT_NE(nullptr, provider);
    RecordingPrepareRendererResources prepRenderer;
    std::vector<RasterOverlayProjection> missingProjections;
    RasterMappedToTilesetTile& mapping =
        tile.rasterOverlayState.ensureMapping(0);
    mapping.update(
        tile.key,
        tile.content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        *provider,
        &prepRenderer,
        missingProjections,
        tile.parent,
        0);
    RasterOverlayTile* loadingTile = mapping.getLoadingTile();
    ASSERT_NE(nullptr, loadingTile);
    loadingTile->setTexture(std::make_unique<DummyTexture>(4, 4));
    mapping.update(
        tile.key,
        tile.content.renderContent.rasterOverlayDetails(),
        256.0,
        256.0,
        *provider,
        &prepRenderer,
        missingProjections,
        tile.parent,
        0);
    ASSERT_EQ(1, prepRenderer.attachCount);

    GpuUploadQueue queue;
    queue.push(PendingGpuUpload{
        key,
        cacheKey,
        GpuReadyData{}});
    std::vector<ActivatedRasterOverlay*> overlays;
    TileEmptyContentRegistry emptyContentRegistry;
    bool resourcesDirty = false;

    const bool processed =
        TilesetContentLifecycleCoordinator::drainGpuUploadQueue(
            TilesetContentUploadContext{
                lifecycle,
                nullptr,
                nullptr,
                &prepRenderer,
                overlays,
                emptyContentRegistry,
                &queue,
                1},
            &budget,
            1,
            [&tile, &key](const TileKey& requestedKey) -> TilesetTile* {
                return requestedKey == key ? &tile : nullptr;
            },
            [&resourcesDirty](TilesetTile&) {
                resourcesDirty = true;
            });

    EXPECT_TRUE(processed);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(queue.hasWork());
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
    EXPECT_FALSE(tile.content.renderContent.asyncGpuUploadPending);
    EXPECT_EQ(TileLoadState::FailedTemporarily, tile.content.loadState);
    ASSERT_TRUE(tile.boundingVolume.has_value());
    EXPECT_EQ(initialBounds, tile.boundingVolume->region);
    EXPECT_DOUBLE_EQ(-100.0, tile.boundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(100.0, tile.boundingVolume->maximumHeight);
    ASSERT_TRUE(tile.contentBoundingVolume.has_value());
    EXPECT_EQ(initialBounds, tile.contentBoundingVolume->region);
    EXPECT_DOUBLE_EQ(-50.0, tile.contentBoundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(50.0, tile.contentBoundingVolume->maximumHeight);
    EXPECT_EQ(0u, tile.rasterOverlayState.mappingCount());
    EXPECT_EQ(1, prepRenderer.detachCount);
}

TEST(GpuUploadQueueTest, ExpiredInteractionBudgetMakesBoundedProgress) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudget setupBudget;
    FrameResourceBudgetConfig setupConfig;
    setupConfig.maxMainThreadFinalizesPerFrame = 3;
    setupBudget.beginFrame(1, setupConfig);

    std::array<std::unique_ptr<TilesetTile>, 3> tiles;
    GpuUploadQueue queue;
    for (size_t i = 0; i < tiles.size(); ++i) {
        const TileKey key{"test", 1, static_cast<int>(i), 0};
        const std::string cacheKey = "budgeted-upload-" + std::to_string(i);
        tiles[i] = std::make_unique<TilesetTile>(
            key,
            Rectangle::fromDegrees(-20.0, -10.0, 20.0, 10.0));
        tiles[i]->content.renderContent.prepareGltfContent(
            std::make_unique<GltfModel>(),
            Mat4::identity());
        tiles[i]->content.renderContent.setTerrainRenderContent(true);
        tiles[i]->markRenderContentLoaded();
        tiles[i]->content.renderContent.asyncGpuUploadPending = true;

        {
            std::lock_guard<std::mutex> lock(lifecycle.mutex());
            lifecycle.pendingLoads().addUpload(PendingTileLoad{
                TileLoadDomain::TerrainContent,
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                static_cast<double>(i),
                TileLoadResult::createRenderableGltfTerrain(
                    std::make_unique<GltfModel>())});
            ASSERT_TRUE(
                lifecycle.pendingLoads()
                    .takeHighestPriorityUpload(setupBudget)
                    .has_value());
        }
        queue.push(PendingGpuUpload{key, cacheKey, GpuReadyData{}});
    }

    FrameResourceBudget budget;
    FrameResourceBudgetConfig config;
    config.mainThreadTimeMs = 0.5;
    config.interactionActive = true;

    std::vector<ActivatedRasterOverlay*> overlays;
    TileEmptyContentRegistry emptyContentRegistry;
    const auto findTile =
        [&tiles](const TileKey& requestedKey) -> TilesetTile* {
        for (const std::unique_ptr<TilesetTile>& tile : tiles) {
            if (tile && tile->key == requestedKey) {
                return tile.get();
            }
        }
        return nullptr;
    };
    const auto drainOneExpiredFrame =
        [&](uint64_t frameNumber) {
        budget.beginFrame(frameNumber, config);
        budget.recordElapsed(FrameResourceLane::TerrainFinalize, 0.5);
        return
        TilesetContentLifecycleCoordinator::drainGpuUploadQueue(
            TilesetContentUploadContext{
                lifecycle,
                nullptr,
                nullptr,
                nullptr,
                overlays,
                emptyContentRegistry,
                &queue,
                frameNumber},
            &budget,
            20,
            findTile,
            [](TilesetTile&) {});
    };

    EXPECT_TRUE(drainOneExpiredFrame(2));
    EXPECT_EQ(1u, queue.size());
    EXPECT_FALSE(tiles[0]->content.renderContent.asyncGpuUploadPending);
    EXPECT_FALSE(tiles[1]->content.renderContent.asyncGpuUploadPending);
    EXPECT_TRUE(tiles[2]->content.renderContent.asyncGpuUploadPending);

    EXPECT_TRUE(drainOneExpiredFrame(3));
    EXPECT_FALSE(queue.hasWork());
    EXPECT_FALSE(tiles[2]->content.renderContent.asyncGpuUploadPending);

    setupBudget.beginFrame(10, setupConfig);
    for (size_t i = 0; i < tiles.size(); ++i) {
        const TileKey& key = tiles[i]->key;
        const std::string cacheKey =
            "idle-catch-up-upload-" + std::to_string(i);
        tiles[i]->content.renderContent.asyncGpuUploadPending = true;
        {
            std::lock_guard<std::mutex> lock(lifecycle.mutex());
            lifecycle.pendingLoads().addUpload(PendingTileLoad{
                TileLoadDomain::TerrainContent,
                key,
                cacheKey,
                TileLoadPriorityGroup::Normal,
                static_cast<double>(i),
                TileLoadResult::createRenderableGltfTerrain(
                    std::make_unique<GltfModel>())});
            ASSERT_TRUE(
                lifecycle.pendingLoads()
                    .takeHighestPriorityUpload(setupBudget)
                    .has_value());
        }
        queue.push(PendingGpuUpload{key, cacheKey, GpuReadyData{}});
    }

    config.interactionActive = false;
    EXPECT_TRUE(drainOneExpiredFrame(11));
    EXPECT_FALSE(queue.hasWork());
    for (const std::unique_ptr<TilesetTile>& tile : tiles) {
        EXPECT_FALSE(tile->content.renderContent.asyncGpuUploadPending);
    }
}
