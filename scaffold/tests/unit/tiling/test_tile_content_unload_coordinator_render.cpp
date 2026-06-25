#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/renderer/IPrepareRendererResources.h"
#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileContentUnloadCoordinator.h"
#include "earth_engine/tiling/TileScheme.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }

private:
    size_t byteSize_ = 0;
};

class DummyTexture final : public Texture {
public:
    DummyTexture(int width, int height) : width_(width), height_(height) {}

    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
};

class RecordingPrepareRendererResources final
    : public IPrepareRendererResources {
public:
    void attachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex,
        std::shared_ptr<const RasterOverlayTile> rasterTile,
        Texture* texture,
        float translationU,
        float translationV,
        float scaleU,
        float scaleV) override {
        ++attachCount;
        lastGeometryKey = geometryKey;
        lastOverlayIndex = overlayIndex;
        lastRasterTile = std::move(rasterTile);
        lastTexture = texture;
        lastUv = {translationU, translationV, scaleU, scaleV};
    }

    void detachRasterInMainThread(
        const TileKey& geometryKey,
        int32_t overlayIndex) noexcept override {
        ++detachCount;
        lastDetachedGeometryKey = geometryKey;
        lastDetachedOverlayIndex = overlayIndex;
    }

    int attachCount = 0;
    int detachCount = 0;
    TileKey lastGeometryKey;
    TileKey lastDetachedGeometryKey;
    int32_t lastOverlayIndex = -1;
    int32_t lastDetachedOverlayIndex = -1;
    std::shared_ptr<const RasterOverlayTile> lastRasterTile;
    Texture* lastTexture = nullptr;
    std::array<float, 4> lastUv{0.0f, 0.0f, 1.0f, 1.0f};
};

std::unique_ptr<GltfModel> makeTriangleGltfModel() {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    primitive.vertices[0].normalEcef = Vec3::unitZ();
    primitive.vertices[1].normalEcef = Vec3::unitZ();
    primitive.vertices[2].normalEcef = Vec3::unitZ();
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f}};
    primitive.indices = {0, 1, 2};
    model->primitives.push_back(std::move(primitive));
    return model;
}

RasterOverlayDetails makeWebMercatorDetails(const Rectangle& rectangle) {
    RasterOverlayDetails details;
    details.rasterOverlayProjections = {RasterOverlayProjection::WebMercator};
    details.rasterOverlayRectangles = {rectangle};
    details.rasterOverlayInvertedVCoordinates = {false};
    details.boundingRegion = {rectangle, 0.0, 0.0};
    return details;
}

} // namespace

TEST(
    TileContentUnloadCoordinatorRenderTest,
    RenderContentClearsTerrainCacheAndRenderResources) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::Render;
    tile.content.loadState = TileLoadState::Done;
    tile.content.renderContent.setMeshReady(true);
    tile.content.renderContent.setSurfaceDrawable(true);
    tile.content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>());
    tile.content.renderContent.setSurfaceGpuBuffers(
        std::make_unique<DummyBuffer>(4),
        nullptr);
    tile.selectionFrameState.updateFrameRenderability(true);

    const std::string cacheKey = "test:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    terrainCache[cacheKey] = std::make_unique<DecodedHeightmap>();
    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert(cacheKey);

    const TileCacheUnloadContentResult result =
        TileContentUnloadCoordinator::unloadContent(
            tile,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);

    EXPECT_EQ(result, TileCacheUnloadContentResult::Remove);
    EXPECT_EQ(terrainCache.find(cacheKey), terrainCache.end());
    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
    EXPECT_EQ(tile.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(tile.content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_FALSE(tile.content.renderContent.isMeshReady());
    EXPECT_FALSE(tile.content.renderContent.isSurfaceDrawable());
    EXPECT_FALSE(tile.selectionFrameState.renderable);
    EXPECT_FALSE(tile.selectionFrameState.completeRenderable);
}

TEST(
    TileContentUnloadCoordinatorRenderTest,
    RenderContentClearsGltfModelAndPrimitiveResources) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::Render;
    tile.content.loadState = TileLoadState::Done;
    tile.content.renderContent.setMeshReady(true);
    tile.content.renderContent.setGltfContent(makeTriangleGltfModel());
    GltfPrimitiveRenderResources resources;
    resources.vertexBuffer = std::make_unique<DummyBuffer>(96);
    resources.indexBuffer = std::make_unique<DummyBuffer>(12);
    resources.vertexCount = 3;
    resources.indexCount = 3;
    tile.content.renderContent.addGltfPrimitiveResource(std::move(resources));
    tile.selectionFrameState.updateFrameRenderability(true);

    const std::string cacheKey = "test:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    TileEmptyContentRegistry emptyContentRegistry;

    const TileCacheUnloadContentResult result =
        TileContentUnloadCoordinator::unloadContent(
            tile,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);

    EXPECT_EQ(result, TileCacheUnloadContentResult::Remove);
    EXPECT_EQ(tile.content.renderContent.gltfModelForRead(), nullptr);
    EXPECT_FALSE(tile.content.renderContent.hasGltfPrimitiveResources());
    EXPECT_EQ(tile.content.contentKind, TileContentKind::Unknown);
    EXPECT_EQ(tile.content.loadState, TileLoadState::Unloaded);
    EXPECT_FALSE(tile.content.renderContent.isMeshReady());
    EXPECT_FALSE(tile.selectionFrameState.renderable);
    EXPECT_FALSE(tile.selectionFrameState.completeRenderable);
}

TEST(
    TileContentUnloadCoordinatorRenderTest,
    ContentLoadingUnloadKeepsInFlightRenderStateLikeCesiumNative) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::Render;
    tile.content.loadState = TileLoadState::ContentLoading;
    tile.content.renderContent.setGltfContent(makeTriangleGltfModel());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.rasterOverlayState.ensureMapping(0);

    const std::string cacheKey = "test:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    terrainCache[cacheKey] = std::make_unique<DecodedHeightmap>();
    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert(cacheKey);

    const TileCacheUnloadContentResult result =
        TileContentUnloadCoordinator::unloadContent(
            tile,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);

    EXPECT_EQ(TileCacheUnloadContentResult::Keep, result);
    EXPECT_EQ(TileContentKind::Render, tile.content.contentKind);
    EXPECT_EQ(TileLoadState::ContentLoading, tile.content.loadState);
    EXPECT_TRUE(tile.content.renderContent.hasGltfContent());
    EXPECT_TRUE(tile.content.renderContent.isTerrainRenderContent());
    EXPECT_EQ(1u, tile.rasterOverlayState.mappingCount());
    EXPECT_NE(terrainCache.end(), terrainCache.find(cacheKey));
    EXPECT_TRUE(emptyContentRegistry.contains(cacheKey));
}

TEST(
    TileContentUnloadCoordinatorRenderTest,
    EmptyContentUnloadRemovesOnlyTileAndClearsEmptyMarkerLikeCesiumNative) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::Empty;
    tile.content.loadState = TileLoadState::Done;
    TilesetTile child(TileKey{"test", 1, 0, 0}, Rectangle{}, &tile);
    tile.children.push_back(&child);

    const std::string cacheKey = "test:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert(cacheKey);

    const TileCacheUnloadContentResult result =
        TileContentUnloadCoordinator::unloadContent(
            tile,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);

    EXPECT_EQ(TileCacheUnloadContentResult::Remove, result);
    EXPECT_EQ(TileContentKind::Unknown, tile.content.contentKind);
    EXPECT_EQ(TileLoadState::Unloaded, tile.content.loadState);
    EXPECT_EQ(1u, tile.children.size());
    EXPECT_EQ(&child, tile.children.front());
    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
}

TEST(
    TileContentUnloadCoordinatorRenderTest,
    ExternalContentUnloadRequestsChildClearLikeCesiumNative) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::External;
    tile.content.loadState = TileLoadState::Done;
    TilesetTile child(TileKey{"test", 1, 0, 0}, Rectangle{}, &tile);
    tile.children.push_back(&child);

    const std::string cacheKey = "test:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    TileEmptyContentRegistry emptyContentRegistry;
    emptyContentRegistry.insert(cacheKey);

    const TileCacheUnloadContentResult result =
        TileContentUnloadCoordinator::unloadContent(
            tile,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);

    EXPECT_EQ(TileCacheUnloadContentResult::RemoveAndClearChildren, result);
    EXPECT_EQ(TileContentKind::Unknown, tile.content.contentKind);
    EXPECT_EQ(TileLoadState::Unloaded, tile.content.loadState);
    EXPECT_FALSE(emptyContentRegistry.contains(cacheKey));
}

TEST(
    TileContentUnloadCoordinatorRenderTest,
    GltfTerrainUnloadDoesNotWaitForNestedUpsampleWork) {
    TilesetTile root(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 180.0, 90.0));
    root.content.contentKind = TileContentKind::Render;
    root.content.loadState = TileLoadState::Done;
    root.content.renderContent.setGltfContent(makeTriangleGltfModel());
    root.content.renderContent.setTerrainRenderContent(true);
    root.content.renderContent.setGltfResourcesReady(true);
    root.selectionFrameState.updateFrameRenderability(true);

    TilesetTile child(
        TileKey{"Geographic-TMS", 1, 0, 0},
        Rectangle::fromDegrees(-180.0, 0.0, 0.0, 90.0),
        &root);
    root.children.push_back(&child);

    TilesetTile grandchild(
        TileKey{"Geographic-TMS", 2, 0, 1},
        Rectangle::fromDegrees(-180.0, 45.0, -90.0, 90.0),
        &child);
    child.children.push_back(&grandchild);
    grandchild.content.markRasterDetailUpsample(
        RasterOverlayProjection::Geographic);
    grandchild.content.loadState = TileLoadState::ContentLoading;

    const std::string cacheKey = "Geographic-TMS:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    TileEmptyContentRegistry emptyContentRegistry;

    const TileCacheUnloadContentResult firstResult =
        TileContentUnloadCoordinator::unloadContent(
            root,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);

    EXPECT_EQ(TileCacheUnloadContentResult::Remove, firstResult);
    EXPECT_EQ(TileLoadState::Unloaded, root.content.loadState);
    EXPECT_FALSE(root.content.renderContent.hasGltfContent());
    EXPECT_FALSE(root.content.renderContent.isTerrainRenderContent());
    EXPECT_FALSE(root.content.renderContent.hasGltfPrimitiveResources());
    EXPECT_FALSE(root.selectionFrameState.renderable);
}

TEST(
    TileContentUnloadCoordinatorRenderTest,
    FailedTemporaryGltfTerrainUnloadWaitsWithoutReleasingResources) {
    TilesetTile root(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 180.0, 90.0));
    root.content.contentKind = TileContentKind::Render;
    root.content.loadState = TileLoadState::FailedTemporarily;
    root.content.renderContent.setGltfContent(makeTriangleGltfModel());
    root.content.renderContent.setTerrainRenderContent(true);
    root.content.renderContent.setGltfResourcesReady(true);
    GltfPrimitiveRenderResources resources;
    resources.vertexBuffer = std::make_unique<DummyBuffer>(96);
    resources.indexBuffer = std::make_unique<DummyBuffer>(12);
    resources.vertexCount = 3;
    resources.indexCount = 3;
    root.content.renderContent.addGltfPrimitiveResource(std::move(resources));

    TilesetTile child(
        TileKey{"Geographic-TMS", 1, 0, 0},
        Rectangle::fromDegrees(-180.0, 0.0, 0.0, 90.0),
        &root);
    root.children.push_back(&child);
    child.content.markRasterDetailUpsample(
        RasterOverlayProjection::Geographic);
    child.content.loadState = TileLoadState::ContentLoading;

    const std::string cacheKey = "Geographic-TMS:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    TileEmptyContentRegistry emptyContentRegistry;

    const TileCacheUnloadContentResult firstResult =
        TileContentUnloadCoordinator::unloadContent(
            root,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);

    EXPECT_EQ(TileCacheUnloadContentResult::Keep, firstResult);
    EXPECT_EQ(TileLoadState::Unloading, root.content.loadState);
    EXPECT_TRUE(root.content.renderContent.hasGltfContent());
    EXPECT_TRUE(root.content.renderContent.hasGltfPrimitiveResources());
    EXPECT_TRUE(root.content.renderContent.isTerrainRenderContent());

    child.content.loadState = TileLoadState::ContentLoaded;

    const TileCacheUnloadContentResult finalResult =
        TileContentUnloadCoordinator::unloadContent(
            root,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            nullptr);

    EXPECT_EQ(TileCacheUnloadContentResult::Remove, finalResult);
    EXPECT_EQ(TileLoadState::Unloaded, root.content.loadState);
    EXPECT_FALSE(root.content.renderContent.hasGltfContent());
    EXPECT_FALSE(root.content.renderContent.hasGltfPrimitiveResources());
    EXPECT_FALSE(root.content.renderContent.isTerrainRenderContent());
}

TEST(
    TileContentUnloadCoordinatorRenderTest,
    ProtectedGltfTerrainUnloadDetachesRasterMappingsBeforeKeepLikeCesiumNative) {
    auto scheme = TileScheme::createXYZWebMercator();
    const TileKey parentKey{scheme->id(), 0, 0, 0};
    const TileKey childKey{scheme->id(), 1, 0, 0};

    TilesetTile parent(parentKey, scheme->tileToRectangle(parentKey));
    parent.content.contentKind = TileContentKind::Render;
    parent.content.loadState = TileLoadState::Done;
    parent.content.renderContent.setMeshReady(true);
    parent.content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>());

    TilesetTile child(
        childKey,
        scheme->tileToRectangle(childKey),
        &parent);
    child.content.markTerrainAvailabilityUpsample();
    child.content.loadState = TileLoadState::ContentLoading;
    parent.children.push_back(&child);

    DebugImageryProvider imagery;
    RasterOverlayTileProvider provider(imagery, *scheme, nullptr);
    provider.setFrameNumber(1);
    RasterOverlayDetails details = makeWebMercatorDetails(parent.bounds);
    RecordingPrepareRendererResources prep;
    std::vector<RasterOverlayProjection> missingProjections;
    RasterMappedToTilesetTile& mapping =
        parent.rasterOverlayState.ensureMapping(0);
    mapping.update(
        parent.key,
        details,
        256.0,
        256.0,
        provider,
        &prep,
        missingProjections);
    ASSERT_NE(nullptr, mapping.getLoadingTile());
    mapping.getLoadingTile()->setTexture(
        std::make_unique<DummyTexture>(8, 4));
    mapping.update(
        parent.key,
        details,
        256.0,
        256.0,
        provider,
        &prep,
        missingProjections);
    ASSERT_EQ(RasterMappedToTilesetTile::State::Attached,
              mapping.getState());
    ASSERT_EQ(1, prep.attachCount);

    const std::string cacheKey = "XYZ-WebMercator:0:0:0";
    std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>
        terrainCache;
    terrainCache[cacheKey] = std::make_unique<DecodedHeightmap>();
    TileEmptyContentRegistry emptyContentRegistry;

    const TileCacheUnloadContentResult result =
        TileContentUnloadCoordinator::unloadContent(
            parent,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            &prep);

    EXPECT_EQ(TileCacheUnloadContentResult::Keep, result);
    EXPECT_EQ(TileContentKind::Render, parent.content.contentKind);
    EXPECT_EQ(TileLoadState::Unloading, parent.content.loadState);
    EXPECT_TRUE(parent.content.renderContent.hasSurfaceMesh());
    EXPECT_EQ(0u, parent.rasterOverlayState.mappingCount());
    EXPECT_EQ(1, prep.detachCount);
    EXPECT_EQ(parent.key, prep.lastDetachedGeometryKey);
    EXPECT_EQ(0, prep.lastDetachedOverlayIndex);
    EXPECT_NE(terrainCache.end(), terrainCache.find(cacheKey));

    const TileCacheUnloadContentResult secondResult =
        TileContentUnloadCoordinator::unloadContent(
            parent,
            cacheKey,
            terrainCache,
            emptyContentRegistry,
            &prep);

    EXPECT_EQ(TileCacheUnloadContentResult::Keep, secondResult);
    EXPECT_EQ(TileContentKind::Render, parent.content.contentKind);
    EXPECT_EQ(TileLoadState::Unloading, parent.content.loadState);
    EXPECT_TRUE(parent.content.renderContent.hasSurfaceMesh());
    EXPECT_EQ(0u, parent.rasterOverlayState.mappingCount());
    EXPECT_EQ(1, prep.detachCount);
    EXPECT_NE(terrainCache.end(), terrainCache.find(cacheKey));
}
