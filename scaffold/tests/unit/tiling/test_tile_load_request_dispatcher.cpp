#include <gtest/gtest.h>

#include "earth_engine/content/GltfContentProvider.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/TileBoundingVolume.h"
#include "earth_engine/tiling/TileLoadLifecycle.h"
#include "earth_engine/tiling/TileLoadRequestDispatcher.h"

#include <condition_variable>
#include <mutex>

using namespace earth_engine;

class DispatcherBudgetTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "dispatcher-budget"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-budget";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TerrainCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    int requestCount = 0;
};

class FanoutTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "fanout-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    int estimatedRequestFanout(const TileKey&) const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://fanout-terrain";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TerrainCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    int requestCount = 0;
};

class DispatcherBudgetContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "dispatcher-budget-content"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey&,
        CancellationToken,
        ContentCallback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        ++requestCount;
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    int requestCount = 0;
};

class SyncTerminalTerrainProvider final : public TerrainProvider {
public:
    explicit SyncTerminalTerrainProvider(bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override { return "dispatcher-terrain-terminal"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-terrain-terminal";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        callback(key, TerrainTileLoadResult::retryLater());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

class SyncEmptyTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override {
        return "dispatcher-empty-terrain";
    }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-empty-terrain";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, TerrainTileLoadResult::empty());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }
};

class SyncUploadTerrainProvider final : public TerrainProvider {
public:
    explicit SyncUploadTerrainProvider(bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override { return "dispatcher-terrain-upload"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-terrain-upload";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        auto heightmap = std::make_unique<DecodedHeightmap>();
        heightmap->tileSize = 2;
        heightmap->heights = {0.0f, 0.0f, 0.0f, 0.0f};
        callback(key, TerrainTileLoadResult::successWithHeightmap(std::move(heightmap)));
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

class DeferredTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "dispatcher-deferred-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-deferred-terrain";
    }
    void requestTile(
        const TileKey&,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        terrainCallback = std::move(callback);
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    TerrainCallback terrainCallback;
};

class RecordingPriorityTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "dispatcher-priority-terrain"; }
    std::string schemeId() const override { return "test"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 1; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://dispatcher-priority-terrain";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority priority = HttpRequestPriority::Normal) override {
        observedPriority = priority;
        callback(key, TerrainTileLoadResult::retryLater());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(
        const uint8_t*,
        size_t) override {
        return nullptr;
    }

    HttpRequestPriority observedPriority = HttpRequestPriority::Normal;
};

class SyncTerminalContentProvider final : public TilesetContentProvider {
public:
    explicit SyncTerminalContentProvider(bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override { return "dispatcher-content-terminal"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        callback(key, TileContentLoadResult::empty());
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

class SyncTerminalMetadataContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override {
        return "dispatcher-content-terminal-metadata";
    }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        TileContentLoadResult result = TileContentLoadResult::external();
        result.metadata.updatedBoundingVolume =
            TileBoundingVolume::fromRegion(
                Rectangle(0.2, 0.3, 0.4, 0.5),
                5.0,
                50.0);
        callback(key, std::move(result));
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }
};

class DeferredContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "dispatcher-deferred-content"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey&,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        contentCallback = std::move(callback);
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    ContentCallback contentCallback;
};

class RecordingPriorityContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "dispatcher-priority-content"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority priority = HttpRequestPriority::Normal) override {
        observedPriority = priority;
        callback(key, TileContentLoadResult::empty());
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    HttpRequestPriority observedPriority = HttpRequestPriority::Normal;
};

class SyncRenderContentProvider final : public TilesetContentProvider {
public:
    explicit SyncRenderContentProvider(bool& issuedBeforeCallback)
        : issuedBeforeCallback_(issuedBeforeCallback) {}

    std::string id() const override { return "dispatcher-content-render"; }
    bool supportsTile(const TileKey&) const override { return true; }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callbackSawIssued = issuedBeforeCallback_;
        callback(
            key,
            TileContentLoadResult::render(std::make_unique<GltfModel>()));
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    bool& issuedBeforeCallback_;
    bool callbackSawIssued = false;
};

TEST(TileLoadRequestDispatcherTest, BlocksWhenBudgetIsExhausted) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 0;
    const TileKey key{"test", 0, 0, 0};

    FrameResourceBudget terrainBudget;
    terrainBudget.beginFrame(1, config);
    DispatcherBudgetTerrainProvider terrainProvider;
    bool terrainIssued = false;

    TileLoadDispatchResult terrainResult =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            terrainBudget,
            terrainProvider,
            key,
            "blocked",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&terrainIssued]() { terrainIssued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Blocked, terrainResult);
    EXPECT_FALSE(terrainIssued);
    EXPECT_EQ(0, terrainProvider.requestCount);
    EXPECT_TRUE(requestState.empty());

    FrameResourceBudget contentBudget;
    contentBudget.beginFrame(1, config);
    DispatcherBudgetContentProvider contentProvider;
    bool contentIssued = false;

    TileLoadDispatchResult contentResult =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            contentBudget,
            contentProvider,
            key,
            "blocked-content",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&contentIssued]() { contentIssued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Blocked, contentResult);
    EXPECT_FALSE(contentIssued);
    EXPECT_EQ(0, contentProvider.requestCount);
    EXPECT_TRUE(requestState.empty());
    EXPECT_FALSE(pendingLoads.hasWork());
    EXPECT_EQ(0u, contentBudget.networkRequestsIssued());
}

TEST(TileLoadRequestDispatcherTest, TerrainFanoutConsumesRequestBudget) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    const TileKey key{"test", 0, 0, 0};

    FrameResourceBudgetConfig blockedConfig;
    blockedConfig.maxNetworkRequestsPerFrame = 1;
    blockedConfig.maxTerrainContentNetworkRequestsPerFrame = 1;
    FrameResourceBudget blockedBudget;
    blockedBudget.beginFrame(1, blockedConfig);
    FanoutTerrainProvider blockedProvider;
    bool blockedIssued = false;

    TileLoadDispatchResult blockedResult =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            blockedBudget,
            blockedProvider,
            key,
            "fanout-blocked",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&blockedIssued]() { blockedIssued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Blocked, blockedResult);
    EXPECT_FALSE(blockedIssued);
    EXPECT_EQ(0, blockedProvider.requestCount);
    EXPECT_EQ(0u, blockedBudget.networkRequestsIssued());
    EXPECT_TRUE(requestState.empty());

    FrameResourceBudgetConfig issuedConfig;
    issuedConfig.maxNetworkRequestsPerFrame = 2;
    issuedConfig.maxTerrainContentNetworkRequestsPerFrame = 2;
    FrameResourceBudget issuedBudget;
    issuedBudget.beginFrame(2, issuedConfig);
    FanoutTerrainProvider issuedProvider;
    bool issued = false;

    TileLoadDispatchResult issuedResult =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            issuedBudget,
            issuedProvider,
            key,
            "fanout-issued",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Issued, issuedResult);
    EXPECT_TRUE(issued);
    EXPECT_EQ(1, issuedProvider.requestCount);
    EXPECT_EQ(2u, issuedBudget.terrainContentNetworkRequestsIssued());
    EXPECT_EQ(2u, issuedBudget.networkRequestsIssued());
    EXPECT_TRUE(requestState.contains("fanout-issued"));
}

TEST(TileLoadRequestDispatcherTest, SkipsEmptyCacheKeys) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;

    DispatcherBudgetTerrainProvider terrainProvider;
    TileLoadDispatchResult terrainResult =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            terrainProvider,
            key,
            "",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    DispatcherBudgetContentProvider contentProvider;
    TileLoadDispatchResult contentResult =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            contentProvider,
            key,
            "",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    TileLoadDispatchResult upsampleResult =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            mutex,
            requestState,
            pendingLoads,
            key,
            "",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadDomain::Terrain,
            TileLoadResult::createRenderable());

    EXPECT_EQ(TileLoadDispatchResult::Skipped, terrainResult);
    EXPECT_EQ(TileLoadDispatchResult::Skipped, contentResult);
    EXPECT_EQ(TileLoadDispatchResult::Skipped, upsampleResult);
    EXPECT_FALSE(issued);
    EXPECT_EQ(0, terrainProvider.requestCount);
    EXPECT_EQ(0, contentProvider.requestCount);
    EXPECT_TRUE(requestState.empty());
    EXPECT_FALSE(pendingLoads.hasWork());
    EXPECT_EQ(0u, budget.networkRequestsIssued());
}

TEST(TileLoadRequestDispatcherTest,
     RunsOnIssuedBeforeSynchronousContentTerminalCallback) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncTerminalContentProvider provider(issued);

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(1u, pendingLoads.contentTerminalResultCount());
}

TEST(TileLoadRequestDispatcherTest,
     RunsOnIssuedBeforeSynchronousTerrainTerminalCallback) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncTerminalTerrainProvider provider(issued);

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "terrain",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(1u, pendingLoads.terrainTerminalResultCount());
}

TEST(TileLoadRequestDispatcherTest,
     TerrainTerminalResultIsStatusOnly) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    SyncEmptyTerrainProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "terrain-empty-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(requestState.empty());
    ASSERT_EQ(1u, pendingLoads.terrainTerminalResultCount());

    auto pending = pendingLoads.takeHighestPriorityTerminalResult(budget);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(TileLoadDomain::Terrain, pending->domain);
    EXPECT_EQ(TileLoadStatus::Empty, pending->result.status);
    EXPECT_FALSE(
        pending->content().metadata.updatedBoundingVolume.has_value());
}

TEST(TileLoadRequestDispatcherTest,
     ContentTerminalResultKeepsTileLoadResultMetadata) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    SyncTerminalMetadataContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content-terminal-metadata",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(requestState.empty());
    ASSERT_EQ(1u, pendingLoads.contentTerminalResultCount());

    auto pending = pendingLoads.takeHighestPriorityTerminalResult(budget);
    ASSERT_TRUE(pending.has_value());
    EXPECT_EQ(TileLoadDomain::Content, pending->domain);
    EXPECT_EQ(TileLoadStatus::External, pending->result.status);
    ASSERT_TRUE(
        pending->content().metadata.updatedBoundingVolume.has_value());
    EXPECT_DOUBLE_EQ(
        5.0,
        pending->content().metadata.updatedBoundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(
        50.0,
        pending->content().metadata.updatedBoundingVolume->maximumHeight);
}

TEST(TileLoadRequestDispatcherTest,
     RunsOnIssuedBeforeSynchronousTerrainUploadCallback) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncUploadTerrainProvider provider(issued);

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(1u, pendingLoads.terrainUploadCount());
    EXPECT_EQ(0u, pendingLoads.contentUploadCount());
    EXPECT_EQ(0u, pendingLoads.terrainTerminalResultCount());
}

TEST(TileLoadRequestDispatcherTest,
     TerrainResultIsHeightmapOnlyAndGltfTerrainUsesContentResult) {
    auto gltfModel = std::make_unique<GltfModel>();
    GltfModel* rawGltfModel = gltfModel.get();
    TileContentLoadResult contentResult =
        TileContentLoadResult::render(std::move(gltfModel));
    contentResult.terrainRenderContent = true;
    contentResult.quantizedMeshAvailabilityUpdates.push_back(
        QuantizedMeshAvailabilityUpdate{});
    TileLoadResult normalizedGltf =
        TileLoadResult::fromContentResult(std::move(contentResult));

    EXPECT_EQ(TileLoadStatus::Renderable, normalizedGltf.status);
    EXPECT_TRUE(normalizedGltf.shouldUpload());
    EXPECT_EQ(TerrainTilePayloadKind::None,
              normalizedGltf.content.terrainPayloadKind);
    EXPECT_TRUE(normalizedGltf.content.terrainRenderContent);
    EXPECT_EQ(nullptr, normalizedGltf.content.heightmap);
    EXPECT_EQ(rawGltfModel, normalizedGltf.content.gltfModel.get());
    EXPECT_EQ(
        1u,
        normalizedGltf.content.quantizedMeshAvailabilityUpdates.size());

    auto heightmap = std::make_unique<DecodedHeightmap>();
    DecodedHeightmap* rawHeightmap = heightmap.get();
    TerrainTileLoadResult heightmapResult =
        TerrainTileLoadResult::successWithHeightmap(std::move(heightmap));

    TileLoadResult normalizedHeightmap =
        TileLoadResult::fromTerrainResult(std::move(heightmapResult));

    EXPECT_EQ(TileLoadStatus::Renderable, normalizedHeightmap.status);
    EXPECT_TRUE(normalizedHeightmap.shouldUpload());
    EXPECT_EQ(TerrainTilePayloadKind::Heightmap,
              normalizedHeightmap.content.terrainPayloadKind);
    EXPECT_TRUE(normalizedHeightmap.content.terrainRenderContent);
    EXPECT_EQ(rawHeightmap, normalizedHeightmap.content.heightmap.get());
    EXPECT_EQ(nullptr, normalizedHeightmap.content.gltfModel);
    EXPECT_FALSE(normalizedHeightmap.content.hasGltfTerrainPayload());
    EXPECT_TRUE(
        normalizedHeightmap.content.quantizedMeshAvailabilityUpdates.empty());

    TileLoadResult untypedTerrain = TileLoadResult::createRenderableTerrain();
    EXPECT_FALSE(untypedTerrain.shouldUpload());

    auto directGltfModel = std::make_unique<GltfModel>();
    GltfModel* rawDirectGltfModel = directGltfModel.get();
    const Rectangle modelRasterRectangle =
        Rectangle::fromDegrees(10.0, 11.0, 12.0, 13.0);
    directGltfModel->rasterOverlayDetails.setGeographicRectangle(
        modelRasterRectangle,
        -3.0,
        4.0);
    TileLoadResultMetadata directMetadata;
    directMetadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(
            Rectangle::fromDegrees(1.0, 2.0, 3.0, 4.0),
            -10.0,
            20.0);
    const Mat4 directTransform = Mat4::translation(Vec3{1.0, 2.0, 3.0});
    TileLoadResult directGltfTerrain =
        TileLoadResult::createRenderableGltfTerrain(
            std::move(directGltfModel),
            directMetadata,
            directTransform);
    EXPECT_TRUE(directGltfTerrain.shouldUpload());
    EXPECT_TRUE(directGltfTerrain.content.hasGltfTerrainPayload());
    EXPECT_EQ(TerrainTilePayloadKind::None,
              directGltfTerrain.content.terrainPayloadKind);
    EXPECT_EQ(nullptr, directGltfTerrain.content.heightmap);
    EXPECT_EQ(rawDirectGltfModel, directGltfTerrain.content.gltfModel.get());
    EXPECT_EQ(directTransform, directGltfTerrain.content.contentTransform);
    ASSERT_TRUE(
        directGltfTerrain.content.metadata.updatedBoundingVolume.has_value());
    const TileBoundingVolume& committedVolume =
        *directGltfTerrain.content.metadata.updatedBoundingVolume;
    EXPECT_EQ(TileBoundingVolumeKind::Region, committedVolume.kind);
    EXPECT_EQ(directMetadata.updatedBoundingVolume->region,
              committedVolume.region);
    EXPECT_DOUBLE_EQ(-10.0, committedVolume.minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, committedVolume.maximumHeight);
    ASSERT_TRUE(
        directGltfTerrain.content.metadata.rasterOverlayDetails.has_value());
    const Rectangle* inheritedRasterRectangle =
        directGltfTerrain.content.metadata.rasterOverlayDetails
            ->findRectangleForOverlayProjection(
                RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, inheritedRasterRectangle);
    EXPECT_EQ(modelRasterRectangle, *inheritedRasterRectangle);

    auto explicitModel = std::make_unique<GltfModel>();
    explicitModel->rasterOverlayDetails.setGeographicRectangle(
        Rectangle::fromDegrees(-40.0, -30.0, -20.0, -10.0));
    TileLoadResultMetadata explicitMetadata;
    const Rectangle explicitRasterRectangle =
        Rectangle::fromDegrees(30.0, 31.0, 32.0, 33.0);
    explicitMetadata.rasterOverlayDetails.emplace();
    explicitMetadata.rasterOverlayDetails->setGeographicRectangle(
        explicitRasterRectangle);
    TileLoadResult explicitGltfTerrain =
        TileLoadResult::createRenderableGltfTerrain(
            std::move(explicitModel),
            explicitMetadata);
    ASSERT_TRUE(
        explicitGltfTerrain.content.metadata.rasterOverlayDetails.has_value());
    const Rectangle* explicitCommittedRectangle =
        explicitGltfTerrain.content.metadata.rasterOverlayDetails
            ->findRectangleForOverlayProjection(
                RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, explicitCommittedRectangle);
    EXPECT_EQ(explicitRasterRectangle, *explicitCommittedRectangle);

    TileLoadResult contentGltf = TileLoadResult::fromContentResult(
        TileContentLoadResult::render(std::make_unique<GltfModel>()));
    EXPECT_TRUE(contentGltf.shouldUpload());
}

TEST(TileLoadRequestDispatcherTest,
     RunsOnIssuedBeforeSynchronousContentUploadCallback) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncRenderContentProvider provider(issued);

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(1u, pendingLoads.contentUploadCount());
    EXPECT_EQ(0u, pendingLoads.contentTerminalResultCount());
}

TEST(TileLoadRequestDispatcherTest, DropsCancelledTerrainUploadCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DeferredTerrainProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestTerrain(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "cancel-terrain",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(issued);
    ASSERT_TRUE(provider.terrainCallback);

    lifecycle.cancelAndEraseCacheKey("cancel-terrain");
    provider.terrainCallback(
        key,
        TerrainTileLoadResult::successWithHeightmap(std::make_unique<DecodedHeightmap>()));

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadRequestDispatcherTest, DropsCancelledContentTerminalCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DeferredContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "cancel-content",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(issued);
    ASSERT_TRUE(provider.contentCallback);

    lifecycle.cancelAndEraseCacheKey("cancel-content");
    provider.contentCallback(key, TileContentLoadResult::empty());

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadRequestDispatcherTest, DropsCancelledTerrainTerminalCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DeferredTerrainProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestTerrain(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "cancel-terrain-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(issued);
    ASSERT_TRUE(provider.terrainCallback);

    lifecycle.cancelAndEraseCacheKey("cancel-terrain-terminal");
    provider.terrainCallback(key, TerrainTileLoadResult::retryLater());

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadRequestDispatcherTest, DropsCancelledContentRenderCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DeferredContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "cancel-content-render",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(issued);
    ASSERT_TRUE(provider.contentCallback);

    lifecycle.cancelAndEraseCacheKey("cancel-content-render");
    provider.contentCallback(
        key,
        TileContentLoadResult::render(std::make_unique<GltfModel>()));

    EXPECT_FALSE(lifecycle.hasPendingWork());
}

TEST(TileLoadRequestDispatcherTest, RejectsRequestsDuringDestroy) {
    TileLoadLifecycle lifecycle;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().markDestroyingAndCancelRequests();
    }

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DispatcherBudgetTerrainProvider terrainProvider;
    DispatcherBudgetContentProvider contentProvider;

    TileLoadDispatchResult terrainResult =
        TileLoadRequestDispatcher::requestTerrain(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            terrainProvider,
            key,
            "destroy-terrain",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });
    TileLoadDispatchResult contentResult =
        TileLoadRequestDispatcher::requestContent(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            contentProvider,
            key,
            "destroy-content",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });
    TileLoadDispatchResult upsampleResult =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            lifecycle.mutex(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            key,
            "destroy-upsample",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadDomain::Terrain,
            TileLoadResult::createRenderable());

    EXPECT_EQ(TileLoadDispatchResult::Destroying, terrainResult);
    EXPECT_EQ(TileLoadDispatchResult::Destroying, contentResult);
    EXPECT_EQ(TileLoadDispatchResult::Destroying, upsampleResult);
    EXPECT_FALSE(issued);
    EXPECT_EQ(0, terrainProvider.requestCount);
    EXPECT_EQ(0, contentProvider.requestCount);
    EXPECT_FALSE(lifecycle.hasPendingWork());
    EXPECT_EQ(0u, budget.networkRequestsIssued());

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().clearAfterCallbacksComplete();
    }
}

TEST(TileLoadRequestDispatcherTest, DropsDestroyingTerrainUploadCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    DeferredTerrainProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestTerrain(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "destroy-terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(provider.terrainCallback);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().markDestroyingAndCancelRequests();
    }

    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {1.0f, 1.0f, 1.0f, 1.0f};
    provider.terrainCallback(
        key,
        TerrainTileLoadResult::successWithHeightmap(std::move(heightmap)));

    EXPECT_FALSE(lifecycle.hasPendingWork());

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().clearAfterCallbacksComplete();
    }
}

TEST(TileLoadRequestDispatcherTest, DropsDestroyingTerrainTerminalCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    DeferredTerrainProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestTerrain(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "destroy-terrain-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(provider.terrainCallback);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().markDestroyingAndCancelRequests();
    }

    provider.terrainCallback(key, TerrainTileLoadResult::retryLater());

    EXPECT_FALSE(lifecycle.hasPendingWork());

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().clearAfterCallbacksComplete();
    }
}

TEST(TileLoadRequestDispatcherTest, DropsDestroyingContentUploadCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    DeferredContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "destroy-content-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(provider.contentCallback);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().markDestroyingAndCancelRequests();
    }

    provider.contentCallback(
        key,
        TileContentLoadResult::render(std::make_unique<GltfModel>()));

    EXPECT_FALSE(lifecycle.hasPendingWork());

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().clearAfterCallbacksComplete();
    }
}

TEST(TileLoadRequestDispatcherTest, DropsDestroyingContentTerminalCallback) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    DeferredContentProvider provider;

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::requestContent(
            lifecycle.mutex(),
            lifecycle.condition(),
            lifecycle.requestState(),
            lifecycle.pendingLoads(),
            budget,
            provider,
            key,
            "destroy-content-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    ASSERT_EQ(TileLoadDispatchResult::Issued, result);
    ASSERT_TRUE(provider.contentCallback);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().markDestroyingAndCancelRequests();
    }

    provider.contentCallback(key, TileContentLoadResult::retryLater());

    EXPECT_FALSE(lifecycle.hasPendingWork());

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().clearAfterCallbacksComplete();
    }
}

TEST(TileLoadRequestDispatcherTest, SkipsPendingTerrainTerminalKeys) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncTerminalTerrainProvider provider(issued);

    TileLoadDispatchResult first =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "terrain-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });
    TileLoadDispatchResult second =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "terrain-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    EXPECT_EQ(TileLoadDispatchResult::Issued, first);
    EXPECT_EQ(TileLoadDispatchResult::Skipped, second);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_EQ(1u, pendingLoads.terrainTerminalResultCount());
    EXPECT_EQ(1u, budget.networkRequestsIssued());
}

TEST(TileLoadRequestDispatcherTest, SkipsPendingContentTerminalKeys) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    SyncTerminalContentProvider provider(issued);

    TileLoadDispatchResult first =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            [&issued]() { issued = true; });
    TileLoadDispatchResult second =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            provider,
            key,
            "content-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            []() {});

    EXPECT_EQ(TileLoadDispatchResult::Issued, first);
    EXPECT_EQ(TileLoadDispatchResult::Skipped, second);
    EXPECT_TRUE(provider.callbackSawIssued);
    EXPECT_EQ(1u, pendingLoads.contentTerminalResultCount());
    EXPECT_EQ(1u, budget.networkRequestsIssued());
}

TEST(TileLoadRequestDispatcherTest, SkipsInflightRequestKeys) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    CancellationToken terrainToken;
    CancellationToken contentToken;

    {
        std::lock_guard<std::mutex> lock(mutex);
        ASSERT_TRUE(requestState.beginTerrainRequest(
            "terrain-inflight",
            terrainToken));
        ASSERT_TRUE(requestState.beginContentRequest(
            "content-inflight",
            contentToken));
    }

    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};
    bool issued = false;
    DispatcherBudgetTerrainProvider terrainProvider;
    DispatcherBudgetContentProvider contentProvider;

    TileLoadDispatchResult terrainResult =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            terrainProvider,
            key,
            "terrain-inflight",
            TileLoadPriorityGroup::Urgent,
            100.0,
            [&issued]() { issued = true; });
    TileLoadDispatchResult contentResult =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            contentProvider,
            key,
            "content-inflight",
            TileLoadPriorityGroup::Urgent,
            100.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Skipped, terrainResult);
    EXPECT_EQ(TileLoadDispatchResult::Skipped, contentResult);
    EXPECT_FALSE(issued);
    EXPECT_EQ(0, terrainProvider.requestCount);
    EXPECT_EQ(0, contentProvider.requestCount);
    EXPECT_EQ(2u, requestState.totalRequestCount());
    EXPECT_FALSE(pendingLoads.hasWork());
    EXPECT_EQ(0u, budget.networkRequestsIssued());

    {
        std::lock_guard<std::mutex> lock(mutex);
        requestState.completeTerrainRequest("terrain-inflight");
        requestState.completeContentRequest("content-inflight");
    }
}

TEST(TileLoadRequestDispatcherTest, SkipsPendingUploadKeys) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};

    {
        std::lock_guard<std::mutex> lock(mutex);
        pendingLoads.addUpload(PendingTileLoad{TileLoadDomain::Terrain,
            key,
            "terrain-upload-pending",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderable()});
        pendingLoads.addUpload(PendingTileLoad{TileLoadDomain::Content,
            key,
            "content-upload-pending",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::fromContentResult(
                TileContentLoadResult::render(
                    std::make_unique<GltfModel>()))});
    }

    bool issued = false;
    DispatcherBudgetTerrainProvider terrainProvider;
    DispatcherBudgetContentProvider contentProvider;

    TileLoadDispatchResult terrainResult =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            terrainProvider,
            key,
            "terrain-upload-pending",
            TileLoadPriorityGroup::Urgent,
            100.0,
            [&issued]() { issued = true; });
    TileLoadDispatchResult contentResult =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            contentProvider,
            key,
            "content-upload-pending",
            TileLoadPriorityGroup::Urgent,
            100.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Skipped, terrainResult);
    EXPECT_EQ(TileLoadDispatchResult::Skipped, contentResult);
    EXPECT_FALSE(issued);
    EXPECT_EQ(0, terrainProvider.requestCount);
    EXPECT_EQ(0, contentProvider.requestCount);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(1u, pendingLoads.terrainUploadCount());
    EXPECT_EQ(1u, pendingLoads.contentUploadCount());
    EXPECT_EQ(0u, budget.networkRequestsIssued());
}

TEST(TileLoadRequestDispatcherTest, SkipsClaimedUploadKeys) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    config.maxMainThreadFinalizesPerFrame = 2;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};

    {
        std::lock_guard<std::mutex> lock(mutex);
        pendingLoads.addUpload(PendingTileLoad{TileLoadDomain::Terrain,
            key,
            "terrain-upload-claimed",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderable()});
        pendingLoads.addUpload(PendingTileLoad{TileLoadDomain::Content,
            key,
            "content-upload-claimed",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::fromContentResult(
                TileContentLoadResult::render(
                    std::make_unique<GltfModel>()))});

        EXPECT_TRUE(
            pendingLoads.takeHighestPriorityUpload(false, budget)
                .has_value());
        EXPECT_TRUE(
            pendingLoads.takeHighestPriorityUpload(false, budget)
                .has_value());
    }

    bool issued = false;
    DispatcherBudgetTerrainProvider terrainProvider;
    DispatcherBudgetContentProvider contentProvider;

    TileLoadDispatchResult terrainResult =
        TileLoadRequestDispatcher::requestTerrain(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            terrainProvider,
            key,
            "terrain-upload-claimed",
            TileLoadPriorityGroup::Urgent,
            100.0,
            [&issued]() { issued = true; });
    TileLoadDispatchResult contentResult =
        TileLoadRequestDispatcher::requestContent(
            mutex,
            condition,
            requestState,
            pendingLoads,
            budget,
            contentProvider,
            key,
            "content-upload-claimed",
            TileLoadPriorityGroup::Urgent,
            100.0,
            [&issued]() { issued = true; });

    EXPECT_EQ(TileLoadDispatchResult::Skipped, terrainResult);
    EXPECT_EQ(TileLoadDispatchResult::Skipped, contentResult);
    EXPECT_FALSE(issued);
    EXPECT_EQ(0, terrainProvider.requestCount);
    EXPECT_EQ(0, contentProvider.requestCount);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(0u, pendingLoads.terrainUploadCount());
    EXPECT_EQ(0u, pendingLoads.contentUploadCount());
    EXPECT_TRUE(pendingLoads.hasWork());
    EXPECT_EQ(0u, budget.networkRequestsIssued());
}

TEST(TileLoadRequestDispatcherTest,
     SkipsUpsampledTerrainWhenCacheKeyPending) {
    std::mutex mutex;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    const TileKey key{"test", 0, 0, 0};

    {
        std::lock_guard<std::mutex> lock(mutex);
        pendingLoads.addTerminalResult(
            PendingTileLoad{
                TileLoadDomain::Terrain,
                key,
                "shared-cache-key",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::RetryLater});
    }

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            mutex,
            requestState,
            pendingLoads,
            key,
            "shared-cache-key",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadDomain::Terrain,
            TileLoadResult::createRenderable());

    EXPECT_EQ(TileLoadDispatchResult::Skipped, result);
    EXPECT_EQ(0u, pendingLoads.terrainUploadCount());
    EXPECT_EQ(1u, pendingLoads.terrainTerminalResultCount());
}

TEST(TileLoadRequestDispatcherTest,
     QueuesUpsampledTerrainWhenNetworkBudgetExhausted) {
    std::mutex mutex;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 0;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            mutex,
            requestState,
            pendingLoads,
            key,
            "upsample-blocked",
            TileLoadPriorityGroup::Urgent,
            100.0,
            TileLoadDomain::Terrain,
            TileLoadResult::createRenderable());

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(1u, pendingLoads.terrainUploadCount());
    EXPECT_EQ(0u, budget.networkRequestsIssued());
}

TEST(TileLoadRequestDispatcherTest,
     QueuesUpsampledContentInContentDomainExplicitly) {
    std::mutex mutex;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    const TileKey key{"test", 1, 0, 0};

    TileLoadDispatchResult result =
        TileLoadRequestDispatcher::queueUpsampledLoad(
            mutex,
            requestState,
            pendingLoads,
            key,
            "content-upsample",
            TileLoadPriorityGroup::Urgent,
            100.0,
            TileLoadDomain::Content,
            TileLoadResult::createRenderableTerrain());

    EXPECT_EQ(TileLoadDispatchResult::Issued, result);
    EXPECT_TRUE(requestState.empty());
    EXPECT_EQ(0u, pendingLoads.terrainUploadCount());
    EXPECT_EQ(1u, pendingLoads.contentUploadCount());
}

TEST(TileLoadRequestDispatcherTest, PassesNetworkPriority) {
    std::mutex mutex;
    std::condition_variable condition;
    TilePendingRequestState requestState;
    TilePendingLoadQueue pendingLoads;
    FrameResourceBudgetConfig config;
    config.maxNetworkRequestsPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    const TileKey key{"test", 0, 0, 0};

    RecordingPriorityTerrainProvider terrainProvider;
    TileLoadRequestDispatcher::requestTerrain(
        mutex,
        condition,
        requestState,
        pendingLoads,
        budget,
        terrainProvider,
        key,
        "priority-terrain",
        TileLoadPriorityGroup::Urgent,
        0.0,
        []() {});

    EXPECT_EQ(HttpRequestPriority::High, terrainProvider.observedPriority);

    RecordingPriorityContentProvider contentProvider;
    TileLoadRequestDispatcher::requestContent(
        mutex,
        condition,
        requestState,
        pendingLoads,
        budget,
        contentProvider,
        key,
        "priority-content",
        TileLoadPriorityGroup::Preload,
        0.0,
        []() {});

    EXPECT_EQ(HttpRequestPriority::Low, contentProvider.observedPriority);
}
