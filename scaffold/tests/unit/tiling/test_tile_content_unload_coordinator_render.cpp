#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/terrain/TerrainTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileContentUnloadCoordinator.h"

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
