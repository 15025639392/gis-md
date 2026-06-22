#include <gtest/gtest.h>

#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/tiling/TileEmptyContentRegistry.h"
#include "earth_engine/tiling/TileContentUploadCommitter.h"
#include "earth_engine/tiling/TilePendingLoadCommitCoordinator.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace earth_engine;

namespace {

class RecordingTerrainContentProvider final : public TilesetContentProvider {
public:
    std::string id() const override { return "recording-terrain-content"; }
    bool supportsTile(const TileKey&) const override { return true; }
    bool providesTerrainQuadtree() const override { return true; }
    void applyTerrainAvailabilityUpdates(
        const std::vector<QuantizedMeshAvailabilityUpdate>& updates) override {
        appliedUpdates.insert(
            appliedUpdates.end(),
            updates.begin(),
            updates.end());
    }
    void requestTileContent(
        const TileKey& key,
        CancellationToken,
        ContentCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, TileContentLoadResult::retryLater());
    }
    TileContentLoadResult decodeContent(const uint8_t*, size_t) override {
        return TileContentLoadResult::failed();
    }

    std::vector<QuantizedMeshAvailabilityUpdate> appliedUpdates;
};

class RecordingTerrainProvider final : public TerrainProvider {
public:
    std::string id() const override { return "recording-terrain"; }
    std::string schemeId() const override { return "Geographic-TMS"; }
    int minZoom() const override { return 0; }
    int maxZoom() const override { return 2; }
    int tileSize() const override { return 2; }
    std::string buildUrl(const TileKey&) const override {
        return "memory://recording-terrain";
    }
    void requestTile(
        const TileKey& key,
        CancellationToken,
        TerrainCallback callback,
        HttpRequestPriority = HttpRequestPriority::Normal) override {
        callback(key, TerrainTileLoadResult::retryLater());
    }
    std::unique_ptr<DecodedHeightmap> decodeTile(const uint8_t*, size_t)
        override {
        return nullptr;
    }
};

TileLoadResultMetadata makeBoundingVolumeMetadata(
    const Rectangle& rectangle,
    double minimumHeight,
    double maximumHeight) {
    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(
            rectangle,
            minimumHeight,
            maximumHeight);
    return metadata;
}

TileLoadResult makeGltfTerrainContentResult(
    std::unique_ptr<GltfModel> model,
    TileLoadResultMetadata metadata = {}) {
    TileContentLoadResult contentResult =
        TileContentLoadResult::render(std::move(model));
    contentResult.terrainRenderContent =
        contentResult.gltfModel != nullptr;
    contentResult.metadata = std::move(metadata);
    return TileLoadResult::fromContentResult(std::move(contentResult));
}

void expectContentTerminalClearsEmptyMarker(TileLoadStatus status) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:0:0:0";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert(cacheKey);
    PendingTileLoad result{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        status};
    bool childrenEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitContentTerminalResult(
        result,
        emptyContentRegistry,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&childrenEnsured](TilesetTile&) { childrenEnsured = true; },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
    EXPECT_TRUE(childrenEnsured);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_EQ(TileContentKind::Unknown, tile.content.contentKind);
    EXPECT_EQ(TileLoadState::FailedTemporarily, tile.content.loadState);
}

void expectTerrainTerminalClearsEmptyMarker(TileLoadStatus status) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:0:0:0";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert(cacheKey);
    PendingTileLoad result{TileLoadDomain::Terrain,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        status};
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitTerrainTerminalResult(
        result,
        emptyContentRegistry,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
    EXPECT_TRUE(resourcesDirty);
    EXPECT_EQ(TileContentKind::Unknown, tile.content.contentKind);
    EXPECT_EQ(TileLoadState::FailedTemporarily, tile.content.loadState);
}

void expectTerrainTerminalIgnoresMetadata(TileLoadStatus status,
                                          TileLoadState expectedLoadState) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "terrain-terminal";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    TileLoadResult result = TileLoadResult::createTerminal(
        status,
        makeBoundingVolumeMetadata(
            Rectangle(0.1, 0.2, 0.3, 0.4),
            -10.0,
            20.0));
    PendingTileLoad pending{TileLoadDomain::Terrain,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        std::move(result)};
    TileEmptyContentRegistry emptyContentRegistry;

    TilePendingLoadCommitCoordinator::commitTerrainTerminalResult(
        pending,
        emptyContentRegistry,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        []() {});

    EXPECT_FALSE(tile.boundingVolume.has_value());
    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
    EXPECT_EQ(expectedLoadState, tile.content.loadState);
}

void expectContentTerminalIgnoresMetadata(TileLoadStatus status,
                                          TileLoadState expectedLoadState) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "content-terminal";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    TileLoadResult result = TileLoadResult::createTerminal(
        status,
        makeBoundingVolumeMetadata(
            Rectangle(0.1, 0.2, 0.3, 0.4),
            -10.0,
            20.0));
    PendingTileLoad pending{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        std::move(result)};
    TileEmptyContentRegistry emptyContentRegistry;

    TilePendingLoadCommitCoordinator::commitContentTerminalResult(
        pending,
        emptyContentRegistry,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](TilesetTile&) {},
        []() {});

    EXPECT_FALSE(tile.boundingVolume.has_value());
    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
    EXPECT_EQ(expectedLoadState, tile.content.loadState);
}

} // namespace

TEST(TilePendingLoadCommitCoordinatorTest,
     MissingTileTerminalResultsHaveNoSideEffects) {
    const TileKey terrainKey{"test", 0, 0, 0};
    const TileKey contentKey{"test", 0, 1, 0};
    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert("missing-terrain");
    emptyContentRegistry.insert("missing-content");
    PendingTileLoad terrainResult{TileLoadDomain::Terrain,
        terrainKey,
        "missing-terrain",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadStatus::RetryLater};
    PendingTileLoad contentResult{TileLoadDomain::Content,
        contentKey,
        "missing-content",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadStatus::RetryLater};
    bool childrenEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitTerrainTerminalResult(
        terrainResult,
        emptyContentRegistry,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [&resourcesDirty]() { resourcesDirty = true; });
    TilePendingLoadCommitCoordinator::commitContentTerminalResult(
        contentResult,
        emptyContentRegistry,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [&childrenEnsured](TilesetTile&) { childrenEnsured = true; },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_FALSE(childrenEnsured);
    EXPECT_FALSE(resourcesDirty);
    EXPECT_TRUE(emptyContentRegistry.contains("missing-terrain"));
    EXPECT_TRUE(emptyContentRegistry.contains("missing-content"));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentEmptyTerminalAppliesTileLoadResultMetadata) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "content-empty";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;
    const Rectangle updatedRectangle(0.1, 0.2, 0.3, 0.4);

    TileLoadResult result = TileLoadResult::createTerminal(
        TileLoadStatus::Empty);
    result.content.metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(updatedRectangle, -10.0, 20.0);
    PendingTileLoad pending{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        std::move(result)};
    TileEmptyContentRegistry emptyContentRegistry;
    bool childrenEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitContentTerminalResult(
        pending,
        emptyContentRegistry,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&childrenEnsured](TilesetTile&) { childrenEnsured = true; },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_TRUE(tile.boundingVolume.has_value());
    EXPECT_EQ(updatedRectangle, tile.boundingVolume->region);
    EXPECT_DOUBLE_EQ(-10.0, tile.boundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(20.0, tile.boundingVolume->maximumHeight);
    EXPECT_FALSE(childrenEnsured);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_TRUE(emptyContentRegistry.contains(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainEmptyTerminalAppliesTileLoadResultMetadata) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "terrain-empty";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;
    const Rectangle updatedRectangle(0.2, 0.3, 0.4, 0.5);

    TileLoadResult result = TileLoadResult::createTerminal(
        TileLoadStatus::Empty,
        makeBoundingVolumeMetadata(updatedRectangle, -30.0, 60.0));
    PendingTileLoad pending{TileLoadDomain::Terrain,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        std::move(result)};
    TileEmptyContentRegistry emptyContentRegistry;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitTerrainTerminalResult(
        pending,
        emptyContentRegistry,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_TRUE(tile.boundingVolume.has_value());
    EXPECT_EQ(updatedRectangle, tile.boundingVolume->region);
    EXPECT_DOUBLE_EQ(-30.0, tile.boundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(60.0, tile.boundingVolume->maximumHeight);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_TRUE(emptyContentRegistry.contains(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentExternalTerminalAppliesTileLoadResultMetadata) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "content-external";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;
    const Rectangle updatedRectangle(0.3, 0.4, 0.5, 0.6);

    TileLoadResult result = TileLoadResult::createTerminal(
        TileLoadStatus::External,
        makeBoundingVolumeMetadata(updatedRectangle, 5.0, 70.0));
    PendingTileLoad pending{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        std::move(result)};
    TileEmptyContentRegistry emptyContentRegistry;
    bool childrenEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitContentTerminalResult(
        pending,
        emptyContentRegistry,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&childrenEnsured](TilesetTile&) { childrenEnsured = true; },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_TRUE(tile.boundingVolume.has_value());
    EXPECT_EQ(updatedRectangle, tile.boundingVolume->region);
    EXPECT_DOUBLE_EQ(5.0, tile.boundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(70.0, tile.boundingVolume->maximumHeight);
    EXPECT_TRUE(childrenEnsured);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentFailedTerminalIgnoresTileLoadResultMetadata) {
    expectContentTerminalIgnoresMetadata(
        TileLoadStatus::Failed,
        TileLoadState::Failed);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainRetryLaterTerminalIgnoresTileLoadResultMetadata) {
    expectTerrainTerminalIgnoresMetadata(
        TileLoadStatus::RetryLater,
        TileLoadState::FailedTemporarily);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     FailedRetryAndCancelledTerminalResultsDropMetadataLikeCesiumNative) {
    for (TileLoadStatus status : {TileLoadStatus::RetryLater,
                                  TileLoadStatus::Failed,
                                  TileLoadStatus::Cancelled}) {
        TileLoadResult result = TileLoadResult::createTerminal(
            status,
            makeBoundingVolumeMetadata(
                Rectangle(0.1, 0.2, 0.3, 0.4),
                -10.0,
                20.0));
        EXPECT_FALSE(result.content.metadata.updatedBoundingVolume.has_value());
    }
}

TEST(TilePendingLoadCommitCoordinatorTest,
     FailedRetryAndCancelledProviderResultsDropMetadataLikeCesiumNative) {
    for (TileLoadStatus status : {TileLoadStatus::RetryLater,
                                  TileLoadStatus::Failed,
                                  TileLoadStatus::Cancelled}) {
        TerrainTileLoadResult terrainResult;
        terrainResult.status = status;
        TileLoadResult normalizedTerrain =
            TileLoadResult::fromTerrainResult(std::move(terrainResult));
        EXPECT_FALSE(
            normalizedTerrain.content.metadata.updatedBoundingVolume
                .has_value());
        EXPECT_TRUE(
            normalizedTerrain.content.quantizedMeshAvailabilityUpdates
                .empty());

        TileContentLoadResult contentResult;
        contentResult.status = status;
        contentResult.metadata = makeBoundingVolumeMetadata(
            Rectangle(0.1, 0.2, 0.3, 0.4),
            -10.0,
            20.0);
        TileLoadResult normalizedContent =
            TileLoadResult::fromContentResult(std::move(contentResult));
        EXPECT_FALSE(
            normalizedContent.content.metadata.updatedBoundingVolume
                .has_value());
    }
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainFailedAndCancelledTerminalsIgnoreTileLoadResultMetadata) {
    expectTerrainTerminalIgnoresMetadata(
        TileLoadStatus::Failed,
        TileLoadState::Failed);
    expectTerrainTerminalIgnoresMetadata(
        TileLoadStatus::Cancelled,
        TileLoadState::FailedTemporarily);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentRetryLaterAndCancelledTerminalsIgnoreTileLoadResultMetadata) {
    expectContentTerminalIgnoresMetadata(
        TileLoadStatus::RetryLater,
        TileLoadState::FailedTemporarily);
    expectContentTerminalIgnoresMetadata(
        TileLoadStatus::Cancelled,
        TileLoadState::FailedTemporarily);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     MissingTileUploadsReleaseCacheKeysWithoutResourceSideEffects) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    PendingTileLoad terrainUpload{TileLoadDomain::Terrain,
        TileKey{"test", 0, 0, 0},
        "missing-terrain",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::createRenderableTerrain()};
    PendingTileLoad contentUpload{TileLoadDomain::Content,
        TileKey{"test", 0, 1, 0},
        "missing-content",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::fromContentResult(TileContentLoadResult::empty())};
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Terrain,
            terrainUpload.key,
            terrainUpload.cacheKey,
            terrainUpload.group,
            terrainUpload.priority,
            TileLoadResult::createRenderableTerrain()});
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            contentUpload.key,
            contentUpload.cacheKey,
            contentUpload.group,
            contentUpload.priority,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
        EXPECT_TRUE(
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget)
                .has_value());
        EXPECT_TRUE(
            lifecycle.pendingLoads().takeHighestPriorityUpload(false, budget)
                .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>> terrainCache;
    bool resourcesDirty = false;
    TilePendingLoadCommitCoordinator::commitTerrainUpload(
        terrainUpload,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        false,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [](TilesetTile&) {},
        [&resourcesDirty]() { resourcesDirty = true; });
    TilePendingLoadCommitCoordinator::commitContentUpload(
        contentUpload,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [](TilesetTile&) {},
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_FALSE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("missing-terrain"));
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("missing-content"));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainUploadCommitterCachesHeightmapWithoutAvailabilitySideEffect) {
    const TileKey key{"test", 1, 2, 3};
    const std::string cacheKey = "terrain-payload";
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {1.0f, 2.0f, 3.0f, 4.0f};

    TileLoadedContent content;
    content.heightmap = std::move(heightmap);
    content.terrainPayloadKind = TerrainTilePayloadKind::Heightmap;

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;

    TileTerrainUploadCommitter::cacheTerrainPayload(
        cacheKey,
        content,
        terrainCache);

    EXPECT_EQ(nullptr, content.heightmap);
    ASSERT_NE(terrainCache.find(cacheKey), terrainCache.end());
    EXPECT_TRUE(terrainCache.at(cacheKey)->valid());
}

TEST(
    TilePendingLoadCommitCoordinatorTest,
    ContentUploadCommitterAppliesTerrainAvailabilityToContentProvider) {
    RecordingTerrainContentProvider contentProvider;

    QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 3;
    update.subtreeKey = TileKey{"Geographic-TMS", 4, 5, 6};
    update.metadataAvailability = {{0, 1, 2, 3, 4}};

    TileLoadedContent content;
    content.terrainRenderContent = true;
    content.gltfModel = std::make_unique<GltfModel>();
    content.quantizedMeshAvailabilityUpdates.push_back(update);

    TileContentUploadCommitter::applyAvailabilityUpdates(
        &contentProvider,
        content);

    ASSERT_EQ(1u, contentProvider.appliedUpdates.size());
    EXPECT_EQ(3, contentProvider.appliedUpdates[0].layerIndex);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentDomainGltfTerrainAppliesAvailabilityAndKeepsTerrainMarker) {
    const TileKey subtreeKey{"Geographic-TMS", 2, 0, 0};
    const TileKey availableChildKey{"Geographic-TMS", 3, 0, 0};
    const TileKey unavailableSiblingKey{"Geographic-TMS", 3, 1, 0};

    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "maxzoom": 10,
      "metadataAvailability": 2
    })json";
    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));
    EXPECT_EQ(TileAvailabilityState::Unknown,
              provider.availabilityState(availableChildKey));

    QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 0;
    update.subtreeKey = subtreeKey;
    update.metadataAvailability = {{0, 0, 0, 0, 0}};

    auto model = std::make_unique<GltfModel>();
    GltfModel* rawModel = model.get();
    TileContentLoadResult contentResult =
        TileContentLoadResult::render(std::move(model));
    contentResult.terrainRenderContent = true;
    contentResult.quantizedMeshAvailabilityUpdates.push_back(update);

    const TileKey key{"Geographic-TMS", 2, 0, 0};
    const std::string cacheKey = "terrain-gltf-content-domain";
    PendingTileLoad upload{
        TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::fromContentResult(std::move(contentResult))};
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{
            TileLoadDomain::Content,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderable()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureMeshCalls = 0;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        &provider,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&ensureMeshCalls](TilesetTile&) { ++ensureMeshCalls; },
        [&ensureGltfCalls](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
            ++ensureGltfCalls;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_EQ(TileAvailabilityState::Available,
              provider.availabilityState(availableChildKey));
    EXPECT_EQ(TileAvailabilityState::NotAvailable,
              provider.availabilityState(unavailableSiblingKey));
    EXPECT_EQ(rawModel, tile.content.renderContent.gltfModelForRead());
    EXPECT_TRUE(tile.content.renderContent.isTerrainRenderContent());
    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_EQ(0, ensureMeshCalls);
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainUploadRejectsGltfTerrainPayload) {
    RecordingTerrainContentProvider provider;

    QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 7;
    update.subtreeKey = TileKey{"Geographic-TMS", 2, 0, 0};
    update.metadataAvailability = {{0, 0, 0, 0, 0}};

    auto model = std::make_unique<GltfModel>();
    GltfModel* rawModel = model.get();
    TileContentLoadResult contentResult =
        TileContentLoadResult::render(std::move(model));
    contentResult.terrainRenderContent = true;
    contentResult.quantizedMeshAvailabilityUpdates.push_back(update);

    const TileKey key{"Geographic-TMS", 2, 0, 0};
    const std::string cacheKey = "terrain-gltf-wrong-domain";
    PendingTileLoad upload{
        TileLoadDomain::Terrain,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::fromContentResult(std::move(contentResult))};
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{
            TileLoadDomain::Terrain,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureMeshCalls = 0;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        &provider,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&ensureMeshCalls](TilesetTile&) { ++ensureMeshCalls; },
        [&ensureGltfCalls](TilesetTile&) { ++ensureGltfCalls; },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_TRUE(provider.appliedUpdates.empty());
    EXPECT_NE(rawModel, tile.content.renderContent.gltfModelForRead());
    EXPECT_FALSE(tile.content.renderContent.hasGltfContent());
    EXPECT_FALSE(tile.content.renderContent.isTerrainRenderContent());
    EXPECT_EQ(TileLoadState::FailedTemporarily, tile.content.loadState);
    EXPECT_EQ(TileContentKind::Unknown, tile.content.contentKind);
    EXPECT_EQ(0, ensureMeshCalls);
    EXPECT_EQ(0, ensureGltfCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_TRUE(terrainCache.empty());
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadAppliesGltfTerrainUpdatedBoundingVolumeToTile) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:0:0:0";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;
    tile.boundingVolume =
        TileBoundingVolume::fromRegion(Rectangle(0.0, 0.0, 1.0, 1.0),
                                       -1000.0,
                                       9000.0);

    auto model = std::make_unique<GltfModel>();
    GltfModel* rawModel = model.get();

    const Rectangle updatedRectangle(0.1, 0.2, 0.3, 0.4);
    const Rectangle updatedContentRectangle(0.5, 0.6, 0.7, 0.8);
    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(updatedRectangle, -25.0, 125.0);
    metadata.updatedContentBoundingVolume =
        TileBoundingVolume::fromRegion(updatedContentRectangle, -5.0, 15.0);
    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        makeGltfTerrainContentResult(
            std::move(model),
            std::move(metadata))};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    bool resourcesDirty = false;
    int ensureGltfCalls = 0;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        true,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](TilesetTile&) {},
        [&ensureGltfCalls](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
            ++ensureGltfCalls;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_EQ(rawModel, tile.content.renderContent.gltfModelForRead());
    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    ASSERT_TRUE(tile.boundingVolume.has_value());
    EXPECT_EQ(TileBoundingVolumeKind::Region, tile.boundingVolume->kind);
    EXPECT_EQ(updatedRectangle, tile.boundingVolume->region);
    EXPECT_DOUBLE_EQ(-25.0, tile.boundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(125.0, tile.boundingVolume->maximumHeight);
    ASSERT_TRUE(tile.contentBoundingVolume.has_value());
    EXPECT_EQ(TileBoundingVolumeKind::Region, tile.contentBoundingVolume->kind);
    EXPECT_EQ(updatedContentRectangle, tile.contentBoundingVolume->region);
    EXPECT_DOUBLE_EQ(-5.0, tile.contentBoundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(15.0, tile.contentBoundingVolume->maximumHeight);
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadAppliesGltfTerrainRasterOverlayDetailsLikeTileLoadResult) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:0:0:0";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    auto model = std::make_unique<GltfModel>();
    GltfModel* rawModel = model.get();

    const Rectangle detailsRectangle =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    RasterOverlayDetails rasterOverlayDetails;
    rasterOverlayDetails.setGeographicRectangle(
        detailsRectangle,
        -25.0,
        125.0);
    TileLoadResultMetadata metadata;
    metadata.rasterOverlayDetails = std::move(rasterOverlayDetails);

    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        makeGltfTerrainContentResult(
            std::move(model),
            std::move(metadata))};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    bool resourcesDirty = false;
    int ensureGltfCalls = 0;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](TilesetTile&) {},
        [&ensureGltfCalls](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
            ++ensureGltfCalls;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_EQ(rawModel, tile.content.renderContent.gltfModelForRead());
    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    const RasterOverlayDetails& committedDetails =
        tile.content.renderContent.rasterOverlayDetails();
    const Rectangle* rectangle =
        committedDetails.findRectangleForOverlayProjection(
            RasterOverlayProjection::Geographic);
    ASSERT_NE(nullptr, rectangle);
    EXPECT_EQ(detailsRectangle, *rectangle);
    EXPECT_EQ(detailsRectangle, committedDetails.boundingRegion.rectangle);
    EXPECT_DOUBLE_EQ(-25.0, committedDetails.boundingRegion.minimumHeight);
    EXPECT_DOUBLE_EQ(125.0, committedDetails.boundingRegion.maximumHeight);
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadCompletesGltfTerrainResourcesWithoutSurfaceMesh) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:gltf-terrain";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    auto model = std::make_unique<GltfModel>();
    GltfModel* rawModel = model.get();
    const Rectangle updatedRectangle(0.1, 0.2, 0.3, 0.4);
    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(updatedRectangle, -30.0, 240.0);
    metadata.horizonOcclusionPoint = Vec3(1.0, 2.0, 3.0);

    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        makeGltfTerrainContentResult(
            std::move(model),
            std::move(metadata))};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureMeshCalls = 0;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&ensureMeshCalls](TilesetTile&) { ++ensureMeshCalls; },
        [&ensureGltfCalls](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
            ++ensureGltfCalls;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_EQ(rawModel, tile.content.renderContent.gltfModelForRead());
    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(tile.content.renderContent.isMeshReady());
    EXPECT_TRUE(tile.content.renderContent.hasGltfResources());
    EXPECT_TRUE(tile.content.renderContent.isGltfRenderReady());
    EXPECT_TRUE(tile.content.renderContent.isRenderContentReady());
    EXPECT_TRUE(tile.content.renderContent.isTerrainRenderContent());
    EXPECT_EQ(TileLoadState::Done, tile.content.loadState);
    EXPECT_EQ(TileContentKind::Render, tile.content.contentKind);
    EXPECT_EQ(0, ensureMeshCalls);
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(tile.content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(-30.0,
                     tile.content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(240.0,
                     tile.content.renderContent.terrainMaximumHeight());
    ASSERT_NE(nullptr, tile.content.renderContent.horizonOcclusionPoint());
    EXPECT_EQ(Vec3(1.0, 2.0, 3.0),
              *tile.content.renderContent.horizonOcclusionPoint());
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadRejectsIncompleteGltfTerrainRenderResources) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:incomplete-gltf-terrain";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    auto model = std::make_unique<GltfModel>();
    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        makeGltfTerrainContentResult(std::move(model))};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureMeshCalls = 0;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&ensureMeshCalls](TilesetTile&) { ++ensureMeshCalls; },
        [&ensureGltfCalls](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            ++ensureGltfCalls;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_FALSE(tile.content.renderContent.hasGltfModel());
    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_EQ(TileLoadState::FailedTemporarily, tile.content.loadState);
    EXPECT_EQ(TileContentKind::Unknown, tile.content.contentKind);
    EXPECT_EQ(0, ensureMeshCalls);
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainUploadWithoutPayloadDoesNotCreateSurfaceMeshFallback) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:terrain-without-payload";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    PendingTileLoad upload{TileLoadDomain::Terrain,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::createRenderableTerrain()};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Terrain,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureMeshCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitTerrainUpload(
        upload,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&ensureMeshCalls](TilesetTile&) { ++ensureMeshCalls; },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_FALSE(tile.content.renderContent.hasGltfModel());
    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(tile.content.renderContent.isTerrainRenderContent());
    EXPECT_EQ(TileLoadState::FailedTemporarily, tile.content.loadState);
    EXPECT_EQ(TileContentKind::Unknown, tile.content.contentKind);
    EXPECT_EQ(0, ensureMeshCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_TRUE(terrainCache.empty());
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     MissingContentUploadPreservesTerrainCache) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "shared-cache-key";
    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::fromContentResult(TileContentLoadResult::empty())};
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            upload.key,
            upload.cacheKey,
            upload.group,
            upload.priority,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    auto cachedHeightmap = std::make_unique<DecodedHeightmap>();
    cachedHeightmap->tileSize = 2;
    cachedHeightmap->heights = {5.0f, 6.0f, 7.0f, 8.0f};
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>> terrainCache;
    terrainCache[cacheKey] = std::move(cachedHeightmap);
    bool gltfEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitContentUpload(
        upload,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [&gltfEnsured](TilesetTile&) { gltfEnsured = true; },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_NE(terrainCache.find(cacheKey), terrainCache.end());
    EXPECT_TRUE(terrainCache.at(cacheKey)->valid());
    EXPECT_FALSE(gltfEnsured);
    EXPECT_FALSE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadPreservesLegacyTerrainCacheForSameKey) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "shared-content-terrain-key";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;
    auto model = std::make_unique<GltfModel>();
    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::fromContentResult(
            TileContentLoadResult::render(std::move(model)))};
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            upload.key,
            upload.cacheKey,
            upload.group,
            upload.priority,
            TileLoadResult::fromContentResult(
                TileContentLoadResult::empty())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    auto cachedHeightmap = std::make_unique<DecodedHeightmap>();
    cachedHeightmap->tileSize = 2;
    cachedHeightmap->heights = {5.0f, 6.0f, 7.0f, 8.0f};
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    terrainCache[cacheKey] = std::move(cachedHeightmap);
    bool gltfEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](TilesetTile&) {},
        [&gltfEnsured](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
            gltfEnsured = true;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_NE(terrainCache.find(cacheKey), terrainCache.end());
    EXPECT_TRUE(terrainCache.at(cacheKey)->valid());
    EXPECT_TRUE(gltfEnsured);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_TRUE(tile.content.renderContent.hasGltfModel());
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadCapturesInitialBoundingVolumesBeforeUpdate) {
    TileLoadLifecycle lifecycle;
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "content-initial-bv";
    TilesetTile tile(key, Rectangle{});
    const Rectangle initialRectangle(0.0, 0.0, 1.0, 1.0);
    const Rectangle initialContentRectangle(0.1, 0.1, 0.9, 0.9);
    tile.boundingVolume =
        TileBoundingVolume::fromRegion(initialRectangle, -1000.0, 9000.0);
    tile.contentBoundingVolume = TileBoundingVolume::fromRegion(
        initialContentRectangle,
        -50.0,
        75.0);
    tile.content.loadState = TileLoadState::ContentLoading;

    auto model = std::make_unique<GltfModel>();
    TileContentLoadResult contentResult = TileContentLoadResult::render(
        std::move(model));
    const Rectangle updatedRectangle(0.25, 0.25, 0.75, 0.75);
    const Rectangle updatedContentRectangle(0.3, 0.3, 0.7, 0.7);
    contentResult.metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(updatedRectangle, -12.0, 34.0);
    contentResult.metadata.updatedContentBoundingVolume =
        TileBoundingVolume::fromRegion(updatedContentRectangle, -5.0, 15.0);
    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::fromContentResult(std::move(contentResult))};
    bool gltfEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitContentUpload(
        upload,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&gltfEnsured](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
            gltfEnsured = true;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_TRUE(tile.initialBoundingVolume.has_value());
    EXPECT_EQ(initialRectangle, tile.initialBoundingVolume->region);
    EXPECT_DOUBLE_EQ(-1000.0, tile.initialBoundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(9000.0, tile.initialBoundingVolume->maximumHeight);
    ASSERT_TRUE(tile.initialContentBoundingVolume.has_value());
    EXPECT_EQ(initialContentRectangle,
              tile.initialContentBoundingVolume->region);
    EXPECT_DOUBLE_EQ(-50.0,
                     tile.initialContentBoundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(75.0,
                     tile.initialContentBoundingVolume->maximumHeight);
    ASSERT_TRUE(tile.boundingVolume.has_value());
    EXPECT_EQ(updatedRectangle, tile.boundingVolume->region);
    ASSERT_TRUE(tile.contentBoundingVolume.has_value());
    EXPECT_EQ(updatedContentRectangle, tile.contentBoundingVolume->region);
    EXPECT_TRUE(gltfEnsured);
    EXPECT_TRUE(resourcesDirty);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentRetryAndCancelledClearEmptyMarker) {
    expectContentTerminalClearsEmptyMarker(TileLoadStatus::RetryLater);
    expectContentTerminalClearsEmptyMarker(TileLoadStatus::Cancelled);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainRetryAndCancelledClearEmptyMarker) {
    expectTerrainTerminalClearsEmptyMarker(TileLoadStatus::RetryLater);
    expectTerrainTerminalClearsEmptyMarker(TileLoadStatus::Cancelled);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     HeightmapTerrainUploadDoesNotApplyQuantizedMeshAvailabilityUpdates) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    const TileKey terrainKey{"Geographic-TMS", 2, 0, 0};
    const TileKey availableChildKey{"Geographic-TMS", 3, 0, 0};
    const TileKey unavailableSiblingKey{"Geographic-TMS", 3, 1, 0};
    const std::string cacheKey = "missing-terrain-with-heightmap";
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {1.0f, 2.0f, 3.0f, 4.0f};

    PendingTileLoad upload{TileLoadDomain::Terrain,
        terrainKey,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::createRenderableHeightmapTerrain(std::move(heightmap))};
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Terrain,
            upload.key,
            upload.cacheKey,
            upload.group,
            upload.priority,
            TileLoadResult::createRenderableTerrain()});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    QuantizedMeshTerrainProvider provider(
        "https://example.invalid/fallback/{z}/{x}/{y}.terrain");
    const std::string layerJson = R"json({
      "format": "quantized-mesh-1.0",
      "projection": "EPSG:4326",
      "scheme": "tms",
      "tiles": ["{z}/{x}/{y}.terrain"],
      "maxzoom": 10,
      "metadataAvailability": 2
    })json";
    ASSERT_TRUE(provider.configureFromLayerJson(
        layerJson,
        "https://example.invalid/layer.json"));
    EXPECT_EQ(TileAvailabilityState::Unknown,
              provider.availabilityState(availableChildKey));

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>> terrainCache;
    bool meshEnsured = false;
    bool resourcesDirty = false;
    TilePendingLoadCommitCoordinator::commitTerrainUpload(
        upload,
        nullptr,
        {},
        terrainCache,
        lifecycle,
        false,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [&meshEnsured](TilesetTile&) { meshEnsured = true; },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_NE(terrainCache.find(cacheKey), terrainCache.end());
    EXPECT_TRUE(terrainCache.at(cacheKey)->valid());
    EXPECT_EQ(TileAvailabilityState::Unknown,
              provider.availabilityState(availableChildKey));
    EXPECT_EQ(TileAvailabilityState::Unknown,
              provider.availabilityState(unavailableSiblingKey));
    EXPECT_FALSE(meshEnsured);
    EXPECT_FALSE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}
