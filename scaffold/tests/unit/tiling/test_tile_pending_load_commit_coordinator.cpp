#include <gtest/gtest.h>

#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTile.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/tiling/TileEmptyContentRegistry.h"
#include "earth_engine/tiling/TileContentUploadCommitter.h"
#include "earth_engine/tiling/TilePendingLoadCommitCoordinator.h"
#include "earth_engine/tiling/TilePendingUploadFrameProcessor.h"
#include "earth_engine/tiling/TileRasterOverlayPrefetcher.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileScheme.h"

#include <array>
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

TileLoadResult makeTerrainContentContentResult(
    std::unique_ptr<GltfModel> model,
    TileLoadResultMetadata metadata = {}) {
    TileContentLoadResult contentResult =
        TileContentLoadResult::renderTerrain(
            std::move(model),
            std::move(metadata));
    return TileLoadResult::fromContentResult(std::move(contentResult));
}

std::unique_ptr<GltfModel> makeCommitCoordinatorQuadTerrainGltfModel(
    const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    primitive.vertices[3].positionEcef = Vec3(1.0, 1.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertices[3].uv = {1.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f},
        std::array<float, 2>{1.0f, 1.0f}};
    primitive.indices = {0, 1, 2, 1, 3, 2};
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
}

std::unique_ptr<GltfModel> makeCartographicQuadTerrainGltfModel(
    const Rectangle& rectangle,
    double minimumHeight,
    double maximumHeight) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const std::array<Cartographic, 4> corners = {
        Cartographic::fromRadians(
            rectangle.west(),
            rectangle.south(),
            minimumHeight),
        Cartographic::fromRadians(
            rectangle.east(),
            rectangle.south(),
            minimumHeight),
        Cartographic::fromRadians(
            rectangle.east(),
            rectangle.north(),
            maximumHeight),
        Cartographic::fromRadians(
            rectangle.west(),
            rectangle.north(),
            maximumHeight)};
    for (const Cartographic& corner : corners) {
        SurfaceVertex vertex;
        vertex.positionEcef = ellipsoid.cartographicToCartesian(corner);
        primitive.vertices.push_back(vertex);
    }
    primitive.indices = {0, 1, 2, 0, 2, 3};
    model->primitives.push_back(std::move(primitive));
    return model;
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
        nullptr,
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
    PendingTileLoad result{TileLoadDomain::TerrainContent,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        status};
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitTerrainTerminalResult(
        result,
        emptyContentRegistry,
        nullptr,
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
    PendingTileLoad pending{TileLoadDomain::TerrainContent,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        std::move(result)};
    TileEmptyContentRegistry emptyContentRegistry;

    TilePendingLoadCommitCoordinator::commitTerrainTerminalResult(
        pending,
        emptyContentRegistry,
        nullptr,
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
        nullptr,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](TilesetTile&) {},
        []() {});

    EXPECT_FALSE(tile.boundingVolume.has_value());
    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
    EXPECT_EQ(expectedLoadState, tile.content.loadState);
}

} // namespace

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainContentTerminalResultsUseTerrainSemanticsLikeCesiumNative) {
    for (const auto& [status, expectedLoadState] :
         std::array<std::pair<TileLoadStatus, TileLoadState>, 2>{
             std::pair{TileLoadStatus::RetryLater,
                       TileLoadState::FailedTemporarily},
             std::pair{TileLoadStatus::Failed, TileLoadState::Failed}}) {
        const TileKey key{"Geographic-TMS", 0, 0, 0};
        const std::string cacheKey =
            status == TileLoadStatus::RetryLater
                ? "gltf-terrain-retry"
                : "gltf-terrain-failed";
        TilesetTile tile(key, Rectangle{});
        tile.content.loadState = TileLoadState::ContentLoading;

        TileEmptyContentRegistry emptyContentRegistry;
        emptyContentRegistry.insert(cacheKey);
        PendingTileLoad result{TileLoadDomain::TerrainContent,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            status};
        bool childrenEnsured = false;
        bool resourcesDirty = false;

        TilePendingLoadCommitCoordinator::commitTerminalResult(
            result,
            emptyContentRegistry,
            nullptr,
            [&tile](const TileKey&) -> TilesetTile* { return &tile; },
            [&childrenEnsured](TilesetTile&) { childrenEnsured = true; },
            [&resourcesDirty]() { resourcesDirty = true; });

        EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
        EXPECT_FALSE(childrenEnsured);
        EXPECT_TRUE(resourcesDirty);
        EXPECT_EQ(TileContentKind::Unknown, tile.content.contentKind);
        EXPECT_EQ(expectedLoadState, tile.content.loadState);
    }
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
        nullptr,
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
        nullptr,
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
     FailedRetryAndCancelledContentResultsDropMetadataLikeCesiumNative) {
    for (TileLoadStatus status : {TileLoadStatus::RetryLater,
                                  TileLoadStatus::Failed,
                                  TileLoadStatus::Cancelled}) {
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
    ContentUploadCommitterAppliesTerrainAvailabilityToContentProvider) {
    RecordingTerrainContentProvider contentProvider;

    QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 3;
    update.subtreeKey = TileKey{"Geographic-TMS", 4, 5, 6};
    update.metadataAvailability = {{0, 1, 2, 3, 4}};

    TileLoadedContent content = TileLoadedContent::fromContentResult(
        TileContentLoadResult::renderTerrain(std::make_unique<GltfModel>()));
    content.quantizedMeshAvailabilityUpdates.push_back(update);

    TileContentUploadCommitter::applyAvailabilityUpdates(
        &contentProvider,
        content);

    ASSERT_EQ(1u, contentProvider.appliedUpdates.size());
    EXPECT_EQ(3, contentProvider.appliedUpdates[0].layerIndex);
    EXPECT_TRUE(content.quantizedMeshAvailabilityUpdatesApplied);

    TileContentUploadCommitter::applyAvailabilityUpdates(
        &contentProvider,
        content);
    EXPECT_EQ(1u, contentProvider.appliedUpdates.size());
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadCommitterSkipsAlreadyAppliedTerrainAvailability) {
    RecordingTerrainContentProvider contentProvider;

    QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 4;
    update.subtreeKey = TileKey{"Geographic-TMS", 4, 5, 6};
    update.metadataAvailability = {{0, 1, 2, 3, 4}};

    TileLoadedContent content = TileLoadedContent::fromContentResult(
        TileContentLoadResult::renderTerrain(std::make_unique<GltfModel>()));
    content.quantizedMeshAvailabilityUpdates.push_back(update);
    content.quantizedMeshAvailabilityUpdatesApplied = true;

    TileContentUploadCommitter::applyAvailabilityUpdates(
        &contentProvider,
        content);

    EXPECT_TRUE(contentProvider.appliedUpdates.empty());
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentDomainTerrainContentAppliesAvailabilityAndKeepsTerrainMarker) {
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
        TileContentLoadResult::renderTerrain(std::move(model));
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
            TileLoadResult::createRenderableGltfTerrain(
                std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        &provider,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
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
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentDomainTerrainContentUploadAppliesContentAvailabilityUpdates) {
    RecordingTerrainContentProvider provider;

    QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 7;
    update.subtreeKey = TileKey{"Geographic-TMS", 2, 0, 0};
    update.metadataAvailability = {{0, 0, 0, 0, 0}};

    auto model = std::make_unique<GltfModel>();
    GltfModel* rawModel = model.get();
    TileContentLoadResult contentResult =
        TileContentLoadResult::renderTerrain(std::move(model));
    contentResult.quantizedMeshAvailabilityUpdates.push_back(update);

    const TileKey key{"Geographic-TMS", 2, 0, 0};
    const std::string cacheKey = "content-gltf-terrain";
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
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        &provider,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&ensureGltfCalls](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
            ++ensureGltfCalls;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_EQ(1u, provider.appliedUpdates.size());
    EXPECT_EQ(update.layerIndex, provider.appliedUpdates.front().layerIndex);
    EXPECT_EQ(update.subtreeKey, provider.appliedUpdates.front().subtreeKey);
    EXPECT_EQ(update.metadataAvailability,
              provider.appliedUpdates.front().metadataAvailability);
    EXPECT_EQ(rawModel, tile.content.renderContent.gltfModelForRead());
    EXPECT_TRUE(tile.content.renderContent.hasGltfContent());
    EXPECT_TRUE(tile.content.renderContent.isTerrainRenderContent());
    EXPECT_EQ(TileLoadState::Done, tile.content.loadState);
    EXPECT_EQ(TileContentKind::Render, tile.content.contentKind);
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_TRUE(terrainCache.empty());
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadAppliesTerrainContentUpdatedBoundingVolumeToTile) {
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
        makeTerrainContentContentResult(
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
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
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
        nullptr,
        {},
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
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
     FailedGltfResourcePreparationRestoresPreUploadBoundingVolumes) {
    const TileKey key{"test", 0, 0, 0};
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    const Rectangle originalRectangle =
        Rectangle::fromDegrees(-20.0, -10.0, 20.0, 10.0);
    const Rectangle originalContentRectangle =
        Rectangle::fromDegrees(-18.0, -8.0, 18.0, 8.0);
    tile.boundingVolume =
        TileBoundingVolume::fromRegion(originalRectangle, -100.0, 100.0);
    tile.contentBoundingVolume =
        TileBoundingVolume::fromRegion(originalContentRectangle, -50.0, 50.0);

    const Rectangle updatedRectangle =
        Rectangle::fromDegrees(1.0, 2.0, 3.0, 4.0);
    const Rectangle updatedContentRectangle =
        Rectangle::fromDegrees(1.5, 2.5, 2.5, 3.5);
    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(updatedRectangle, 10.0, 20.0);
    metadata.updatedContentBoundingVolume =
        TileBoundingVolume::fromRegion(updatedContentRectangle, 12.0, 18.0);
    TileLoadResult loadResult = makeTerrainContentContentResult(
        std::make_unique<GltfModel>(),
        std::move(metadata));

    TileContentUploadCommitter::prepareRenderContent(
        tile,
        std::move(loadResult.content));
    tile.rasterOverlayState.ensureMapping(0);
    tile.rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection::WebMercator);
    ASSERT_EQ(1u, tile.rasterOverlayState.mappingCount());
    ASSERT_TRUE(tile.rasterOverlayState.hasMissingProjections());
    ASSERT_TRUE(tile.boundingVolume.has_value());
    EXPECT_EQ(updatedRectangle, tile.boundingVolume->region);
    ASSERT_TRUE(tile.contentBoundingVolume.has_value());
    EXPECT_EQ(updatedContentRectangle, tile.contentBoundingVolume->region);

    TileContentUploadCommitAction action =
        TileContentUploadCommitter::finishRenderResourcePreparation(
            tile,
            false);

    EXPECT_TRUE(action.resourcesDirty);
    EXPECT_FALSE(tile.content.renderContent.hasGltfModel());
    EXPECT_EQ(TileLoadState::FailedTemporarily, tile.content.loadState);
    EXPECT_EQ(TileContentKind::Unknown, tile.content.contentKind);
    EXPECT_EQ(0u, tile.rasterOverlayState.mappingCount());
    EXPECT_FALSE(tile.rasterOverlayState.hasMissingProjections());
    ASSERT_TRUE(tile.boundingVolume.has_value());
    EXPECT_EQ(originalRectangle, tile.boundingVolume->region);
    EXPECT_DOUBLE_EQ(-100.0, tile.boundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(100.0, tile.boundingVolume->maximumHeight);
    ASSERT_TRUE(tile.contentBoundingVolume.has_value());
    EXPECT_EQ(originalContentRectangle, tile.contentBoundingVolume->region);
    EXPECT_DOUBLE_EQ(-50.0, tile.contentBoundingVolume->minimumHeight);
    EXPECT_DOUBLE_EQ(50.0, tile.contentBoundingVolume->maximumHeight);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadGeneratesRasterDetailsFromUpdatedBoundsBeforeTileContentBounds) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:gltf-terrain-updated-bounds";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    const Rectangle staleContentRectangle =
        Rectangle::fromDegrees(-80.0, -30.0, -70.0, -20.0);
    tile.contentBoundingVolume =
        TileBoundingVolume::fromRegion(staleContentRectangle, -10.0, 10.0);

    const Rectangle updatedRectangle =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(updatedRectangle, -25.0, 125.0);

    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        makeTerrainContentContentResult(
            std::make_unique<GltfModel>(),
            std::move(metadata))};

    RasterOverlay overlay(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activeOverlay(overlay);
    std::vector<ActivatedRasterOverlay*> rasterOverlays{&activeOverlay};

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
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        nullptr,
        rasterOverlays,
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    const RasterOverlayDetails& committedDetails =
        tile.content.renderContent.rasterOverlayDetails();
    const Rectangle* rectangle =
        committedDetails.findRectangleForOverlayProjection(
            RasterOverlayProjection::WebMercator);
    ASSERT_NE(nullptr, rectangle);

    const Rectangle expectedUpdatedProjection = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        updatedRectangle);
    const Rectangle staleContentProjection = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        staleContentRectangle);
    EXPECT_EQ(expectedUpdatedProjection, *rectangle);
    EXPECT_NE(staleContentProjection, *rectangle);
    EXPECT_EQ(updatedRectangle, committedDetails.boundingRegion.rectangle);
    EXPECT_FALSE(tile.contentBoundingVolume.has_value());
    ASSERT_TRUE(tile.initialContentBoundingVolume.has_value());
    EXPECT_EQ(staleContentRectangle,
              tile.initialContentBoundingVolume->region);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadClearsStaleRasterOverlayStateBeforeNewRenderGeneration) {
    const TileKey key{"Geographic-TMS", 2, 1, 1};
    const std::string cacheKey = "test:gltf-raster-generation";
    const Rectangle bounds = Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0);
    TilesetTile tile(key, bounds);
    tile.content.loadState = TileLoadState::ContentLoading;
    tile.geometricError = 100.0;
    tile.rasterOverlayState.ensureMapping(0);
    tile.rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection::WebMercator);

    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(bounds, -25.0, 125.0);

    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        makeTerrainContentContentResult(
            std::make_unique<GltfModel>(),
            std::move(metadata))};

    RasterOverlay overlay(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createXYZWebMercator(),
        RasterOverlay::Options{});
    ActivatedRasterOverlay activeOverlay(overlay);
    std::vector<ActivatedRasterOverlay*> rasterOverlays{&activeOverlay};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    config.maxRasterNetworkRequestsPerFrame = 64;
    config.maxRasterNetworkInflight = 64;
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
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        nullptr,
        rasterOverlays,
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_TRUE(tile.rasterOverlayState.mappings().empty());
    EXPECT_TRUE(tile.rasterOverlayState.missingProjections().empty());

    budget.beginFrame(2, config);
    TileRasterOverlayPrefetcher::prefetch(
        tile,
        rasterOverlays,
        {0},
        nullptr,
        16.0,
        budget);

    EXPECT_TRUE(tile.rasterOverlayState.missingProjections().empty());
    RasterMappedToTilesetTile* mapped = tile.rasterOverlayState.mappingAt(0);
    ASSERT_NE(nullptr, mapped);
    ASSERT_NE(nullptr, mapped->getLoadingTile());
    EXPECT_NE(RasterOverlayTile::LoadState::Placeholder,
              mapped->getLoadingTile()->getState());
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadCompletesTerrainContentResourcesWithoutSurfaceMesh) {
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
        makeTerrainContentContentResult(
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
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
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
     ContentUploadTightensLooseGltfTerrainBoundsWithoutRasterOverlays) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:gltf-terrain-tight-bounds";
    const Rectangle looseRectangle = Rectangle::MAXIMUM;
    TilesetTile tile(key, looseRectangle);
    tile.content.loadState = TileLoadState::ContentLoading;
    tile.boundingVolume =
        TileBoundingVolume::fromRegion(looseRectangle, -1000.0, 9000.0);

    const Rectangle modelRectangle =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    auto model = makeCartographicQuadTerrainGltfModel(
        modelRectangle,
        -25.0,
        125.0);
    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        makeTerrainContentContentResult(std::move(model))};

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
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    bool resourcesDirty = false;
    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_TRUE(tile.initialBoundingVolume.has_value());
    EXPECT_EQ(looseRectangle, tile.initialBoundingVolume->region);
    ASSERT_TRUE(tile.boundingVolume.has_value());
    EXPECT_EQ(TileBoundingVolumeKind::Region, tile.boundingVolume->kind);
    EXPECT_TRUE(tile.boundingVolume->region.equalsEpsilon(
        modelRectangle,
        1e-12));
    EXPECT_NEAR(-25.0, tile.boundingVolume->minimumHeight, 1e-5);
    EXPECT_NEAR(125.0, tile.boundingVolume->maximumHeight, 1e-5);
    EXPECT_TRUE(tile.content.renderContent.hasTerrainHeightRange());
    EXPECT_NEAR(-25.0,
                tile.content.renderContent.terrainMinimumHeight(),
                1e-5);
    EXPECT_NEAR(125.0,
                tile.content.renderContent.terrainMaximumHeight(),
                1e-5);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadRejectsIncompleteTerrainContentRenderResources) {
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
        makeTerrainContentContentResult(std::move(model))};

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
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
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
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentDomainTerrainContentUploadUsesContentLifecycleLikeCesiumNative) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:gltf-terrain-content-domain";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    auto model = std::make_unique<GltfModel>();
    const GltfModel* rawModel = model.get();
    TileLoadResultMetadata metadata;
    metadata.terrainHeightRange = {-45.0, 345.0};
    metadata.horizonOcclusionPoint = Vec3(4.0, 5.0, 6.0);
    PendingTileLoad upload{TileLoadDomain::Content,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        makeTerrainContentContentResult(std::move(model), std::move(metadata))};

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
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&ensureGltfCalls](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
            ++ensureGltfCalls;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_TRUE(terrainCache.empty());
    EXPECT_EQ(rawModel, tile.content.renderContent.gltfModelForRead());
    EXPECT_TRUE(tile.content.renderContent.isTerrainRenderContent());
    EXPECT_TRUE(tile.content.renderContent.isGltfRenderReady());
    EXPECT_EQ(TileLoadState::Done, tile.content.loadState);
    EXPECT_EQ(TileContentKind::Render, tile.content.contentKind);
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(tile.content.renderContent.hasTerrainHeightRange());
    EXPECT_DOUBLE_EQ(-45.0,
                     tile.content.renderContent.terrainMinimumHeight());
    EXPECT_DOUBLE_EQ(345.0,
                     tile.content.renderContent.terrainMaximumHeight());
    ASSERT_NE(nullptr, tile.content.renderContent.horizonOcclusionPoint());
    EXPECT_EQ(Vec3(4.0, 5.0, 6.0),
              *tile.content.renderContent.horizonOcclusionPoint());
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     ContentUploadWithoutPayloadDoesNotMaterializeTerrainContentUpsample) {
    const TileKey parentKey{"test", 0, 0, 0};
    const TileKey childKey{"test", 1, 0, 0};
    const std::string cacheKey = "test:gltf-upsample-without-payload";
    const Rectangle parentBounds{-1.0, -0.5, 1.0, 0.5};
    const Rectangle childBounds{-1.0, -0.5, 0.0, 0.0};
    TilesetTile parent(parentKey, parentBounds);
    TilesetTile child(childKey, childBounds, &parent);
    child.content.loadState = TileLoadState::ContentLoading;
    child.content.upsampledFromParent = true;
    parent.content.renderContent.setGltfContent(
        makeCommitCoordinatorQuadTerrainGltfModel(parentBounds));
    parent.content.renderContent.setTerrainRenderContent(true);
    parent.markRenderContentDone();

    PendingTileLoad upload{TileLoadDomain::Content,
        childKey,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            childKey,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    bool gltfEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [&child](const TileKey&) -> TilesetTile* { return &child; },
        [&gltfEnsured](TilesetTile&) { gltfEnsured = true; },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_TRUE(gltfEnsured);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(child.content.renderContent.hasGltfModel());
    EXPECT_FALSE(child.content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(child.content.renderContent.isTerrainRenderContent());
    EXPECT_EQ(TileLoadState::FailedTemporarily, child.content.loadState);
    EXPECT_EQ(TileContentKind::Unknown, child.content.contentKind);
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
     TerrainContentDomainUploadUsesContentLifecycleWithoutLegacyMeshPath) {
    const TileKey key{"Geographic-TMS", 2, 0, 0};
    const std::string cacheKey = "gltf-terrain-domain-content-upload";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    auto model = std::make_unique<GltfModel>();
    const GltfModel* rawModel = model.get();
    PendingTileLoad upload{
        TileLoadDomain::TerrainContent,
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        makeTerrainContentContentResult(std::move(model))};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addUpload(PendingTileLoad{
            TileLoadDomain::TerrainContent,
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableGltfTerrain(std::make_unique<GltfModel>())});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    int ensureGltfCalls = 0;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitUpload(
        upload,
        nullptr,
        nullptr,
        nullptr,
        {},
        lifecycle,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [&ensureGltfCalls](TilesetTile& committedTile) {
            committedTile.content.renderContent.addGltfPrimitiveResource(
                GltfPrimitiveRenderResources{});
            committedTile.markRenderContentDone();
            ++ensureGltfCalls;
        },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_TRUE(terrainCache.empty());
    EXPECT_EQ(rawModel, tile.content.renderContent.gltfModelForRead());
    EXPECT_TRUE(tile.content.renderContent.isTerrainRenderContent());
    EXPECT_TRUE(tile.content.renderContent.isGltfRenderReady());
    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_EQ(TileLoadState::Done, tile.content.loadState);
    EXPECT_EQ(1, ensureGltfCalls);
    EXPECT_TRUE(resourcesDirty);
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
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    bool gltfEnsured = false;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitContentUpload(
        upload,
        nullptr,
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
