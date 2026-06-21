#include <gtest/gtest.h>

#include "earth_engine/providers/QuantizedMeshTerrainProvider.h"
#include "earth_engine/tiling/TileEmptyContentRegistry.h"
#include "earth_engine/tiling/TilePendingLoadCommitCoordinator.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"

#include <memory>
#include <unordered_map>

using namespace earth_engine;

namespace {

void expectContentTerminalClearsEmptyMarker(TileContentLoadStatus status) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:0:0:0";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert(cacheKey);
    PendingContentTerminalResult result{
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

void expectTerrainTerminalClearsEmptyMarker(TerrainTileLoadStatus status) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:0:0:0";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert(cacheKey);
    PendingTerrainTerminalResult result{
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

} // namespace

TEST(TilePendingLoadCommitCoordinatorTest,
     MissingTileTerminalResultsHaveNoSideEffects) {
    const TileKey terrainKey{"test", 0, 0, 0};
    const TileKey contentKey{"test", 0, 1, 0};
    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert("missing-terrain");
    emptyContentRegistry.insert("missing-content");
    PendingTerrainTerminalResult terrainResult{
        terrainKey,
        "missing-terrain",
        TileLoadPriorityGroup::Normal,
        0.0,
        TerrainTileLoadStatus::RetryLater};
    PendingContentTerminalResult contentResult{
        contentKey,
        "missing-content",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileContentLoadStatus::RetryLater};
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
     MissingTileUploadsReleaseCacheKeysWithoutResourceSideEffects) {
    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 4;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);

    PendingTerrainUpload terrainUpload{
        TileKey{"test", 0, 0, 0},
        "missing-terrain",
        TileLoadPriorityGroup::Normal,
        0.0,
        nullptr,
        nullptr};
    PendingContentUpload contentUpload{
        TileKey{"test", 0, 1, 0},
        "missing-content",
        TileLoadPriorityGroup::Normal,
        0.0,
        TileContentLoadResult::empty()};
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            terrainUpload.key,
            terrainUpload.cacheKey,
            terrainUpload.group,
            terrainUpload.priority,
            nullptr,
            nullptr});
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            contentUpload.key,
            contentUpload.cacheKey,
            contentUpload.group,
            contentUpload.priority,
            TileContentLoadResult::empty()});
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
        terrainCache,
        lifecycle,
        false,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [](const TileKey&, const DecodedHeightmap*, const SurfaceTileMesh*) {},
        [](TilesetTile&) {},
        [&resourcesDirty]() { resourcesDirty = true; });
    TilePendingLoadCommitCoordinator::commitContentUpload(
        contentUpload,
        terrainCache,
        lifecycle,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [](TilesetTile&) {},
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_FALSE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("missing-terrain"));
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey("missing-content"));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainUploadConsumesSurfaceMeshAsLoadResultPayload) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:0:0:0";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    auto surfaceMesh = std::make_unique<SurfaceTileMesh>();
    SurfaceVertex vertex;
    vertex.positionEcef = Vec3(1.0, 0.0, 0.0);
    surfaceMesh->vertices.push_back(vertex);
    SurfaceTileMesh* rawSurfaceMesh = surfaceMesh.get();

    PendingTerrainUpload upload{
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        nullptr,
        std::move(surfaceMesh)};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            nullptr,
            nullptr});
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
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](const TileKey&, const DecodedHeightmap*, const SurfaceTileMesh*) {},
        [&ensureMeshCalls](TilesetTile&) { ++ensureMeshCalls; },
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_TRUE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_EQ(rawSurfaceMesh, tile.content.renderContent.surfaceMesh());
    EXPECT_TRUE(terrainCache.empty());
    EXPECT_EQ(0, ensureMeshCalls);
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainUploadAppliesUpdatedBoundingVolumeToTile) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:0:0:0";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;
    tile.boundingVolume =
        TileBoundingVolume::fromRegion(Rectangle(0.0, 0.0, 1.0, 1.0),
                                       -1000.0,
                                       9000.0);

    auto surfaceMesh = std::make_unique<SurfaceTileMesh>();
    SurfaceVertex vertex;
    vertex.positionEcef = Vec3(1.0, 0.0, 0.0);
    surfaceMesh->vertices.push_back(vertex);

    const Rectangle updatedRectangle(0.1, 0.2, 0.3, 0.4);
    const Rectangle updatedContentRectangle(0.5, 0.6, 0.7, 0.8);
    TileLoadResultMetadata metadata;
    metadata.updatedBoundingVolume =
        TileBoundingVolume::fromRegion(updatedRectangle, -25.0, 125.0);
    metadata.updatedContentBoundingVolume =
        TileBoundingVolume::fromRegion(updatedContentRectangle, -5.0, 15.0);
    PendingTerrainUpload upload{
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        nullptr,
        std::move(surfaceMesh),
        {},
        std::move(metadata)};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            nullptr,
            nullptr});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitTerrainUpload(
        upload,
        nullptr,
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](const TileKey&, const DecodedHeightmap*, const SurfaceTileMesh*) {},
        [](TilesetTile&) {},
        [&resourcesDirty]() { resourcesDirty = true; });

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
    EXPECT_TRUE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainUploadAppliesRasterOverlayDetailsLikeTileLoadResult) {
    const TileKey key{"test", 0, 0, 0};
    const std::string cacheKey = "test:0:0:0";
    TilesetTile tile(key, Rectangle{});
    tile.content.loadState = TileLoadState::ContentLoading;

    auto surfaceMesh = std::make_unique<SurfaceTileMesh>();
    SurfaceVertex vertex;
    vertex.positionEcef = Vec3(1.0, 0.0, 0.0);
    surfaceMesh->vertices.push_back(vertex);

    const Rectangle detailsRectangle =
        Rectangle::fromDegrees(-12.0, -4.0, -6.0, 2.0);
    RasterOverlayDetails rasterOverlayDetails;
    rasterOverlayDetails.setGeographicRectangle(
        detailsRectangle,
        -25.0,
        125.0);
    TileLoadResultMetadata metadata;
    metadata.rasterOverlayDetails = std::move(rasterOverlayDetails);

    PendingTerrainUpload upload{
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        nullptr,
        std::move(surfaceMesh),
        {},
        std::move(metadata)};

    TileLoadLifecycle lifecycle;
    FrameResourceBudgetConfig config;
    config.maxMainThreadFinalizesPerFrame = 1;
    FrameResourceBudget budget;
    budget.beginFrame(1, config);
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            key,
            cacheKey,
            TileLoadPriorityGroup::Normal,
            0.0,
            nullptr,
            nullptr});
        ASSERT_TRUE(lifecycle.pendingLoads()
                        .takeHighestPriorityUpload(false, budget)
                        .has_value());
    }

    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    bool resourcesDirty = false;

    TilePendingLoadCommitCoordinator::commitTerrainUpload(
        upload,
        nullptr,
        terrainCache,
        lifecycle,
        false,
        [&tile](const TileKey&) -> TilesetTile* { return &tile; },
        [](const TileKey&, const DecodedHeightmap*, const SurfaceTileMesh*) {},
        [](TilesetTile&) {},
        [&resourcesDirty]() { resourcesDirty = true; });

    ASSERT_TRUE(tile.content.renderContent.hasSurfaceMesh());
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
    EXPECT_TRUE(resourcesDirty);
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
    PendingContentUpload upload{
        key,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        TileContentLoadResult::empty()};
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addContentUpload(PendingContentUpload{
            upload.key,
            upload.cacheKey,
            upload.group,
            upload.priority,
            TileContentLoadResult::empty()});
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
        terrainCache,
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
     ContentRetryAndCancelledClearEmptyMarker) {
    expectContentTerminalClearsEmptyMarker(TileContentLoadStatus::RetryLater);
    expectContentTerminalClearsEmptyMarker(TileContentLoadStatus::Cancelled);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     TerrainRetryAndCancelledClearEmptyMarker) {
    expectTerrainTerminalClearsEmptyMarker(TerrainTileLoadStatus::RetryLater);
    expectTerrainTerminalClearsEmptyMarker(TerrainTileLoadStatus::Cancelled);
}

TEST(TilePendingLoadCommitCoordinatorTest,
     MissingTerrainUploadCachesAndIngestsAvailability) {
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
    QuantizedMeshAvailabilityUpdate update;
    update.layerIndex = 0;
    update.subtreeKey = terrainKey;
    update.metadataAvailability = {{0, 0, 0, 0, 0}};

    PendingTerrainUpload upload{
        terrainKey,
        cacheKey,
        TileLoadPriorityGroup::Normal,
        0.0,
        std::move(heightmap),
        nullptr,
        {update}};
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.pendingLoads().addTerrainUpload(PendingTerrainUpload{
            upload.key,
            upload.cacheKey,
            upload.group,
            upload.priority,
            nullptr,
            nullptr});
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
    int availabilityIngests = 0;
    bool meshEnsured = false;
    bool resourcesDirty = false;
    TilePendingLoadCommitCoordinator::commitTerrainUpload(
        upload,
        &provider,
        terrainCache,
        lifecycle,
        false,
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [&availabilityIngests, &terrainKey](const TileKey& key,
                                            const DecodedHeightmap* hm,
                                            const SurfaceTileMesh*) {
            EXPECT_EQ(terrainKey, key);
            ASSERT_NE(nullptr, hm);
            EXPECT_TRUE(hm->valid());
            ++availabilityIngests;
        },
        [&meshEnsured](TilesetTile&) { meshEnsured = true; },
        [&resourcesDirty]() { resourcesDirty = true; });

    EXPECT_EQ(1, availabilityIngests);
    ASSERT_NE(terrainCache.find(cacheKey), terrainCache.end());
    EXPECT_TRUE(terrainCache.at(cacheKey)->valid());
    EXPECT_EQ(TileAvailabilityState::Available,
              provider.availabilityState(availableChildKey));
    EXPECT_EQ(TileAvailabilityState::NotAvailable,
              provider.availabilityState(unavailableSiblingKey));
    EXPECT_FALSE(meshEnsured);
    EXPECT_FALSE(resourcesDirty);
    EXPECT_FALSE(lifecycle.containsWorkForCacheKey(cacheKey));
}
