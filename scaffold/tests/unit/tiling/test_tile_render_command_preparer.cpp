#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileRenderCommandPreparer.h"

#include <array>
#include <memory>
#include <vector>

using namespace earth_engine;

namespace {

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }

private:
    size_t byteSize_ = 0;
};

TileRenderCommandPrepareContext makeContext(
    bool allowSynchronousMeshPrep) {
    TileRenderCommandPrepareContext context;
    context.frameNumber = 31;
    context.generation = 9;
    context.currentFrameTimeSeconds = 1.25;
    context.maximumScreenSpaceError = 3.5;
    context.transitionOpacity = 0.5f;
    context.allowSynchronousMeshPrep = allowSynchronousMeshPrep;
    context.surfaceClipUv = std::array<float, 4>{0.25f, 0.0f, 0.5f, 1.0f};
    return context;
}

} // namespace

TEST(TileRenderCommandPreparerTest, DefersMeshPrepWhenSynchronousPrepDisabled) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    FrameResourceBudget budget;
    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    bool ensureMeshCalled = false;
    bool unloadContentCalled = false;
    bool upsampleChildrenCalled = false;

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        budget,
        makeContext(false),
        [&ensureMeshCalled](TilesetTile&) {
            ensureMeshCalled = true;
        },
        [&unloadContentCalled](TilesetTile&) {
            unloadContentCalled = true;
        },
        [&upsampleChildrenCalled](TilesetTile&) {
            upsampleChildrenCalled = true;
        });

    EXPECT_FALSE(ensureMeshCalled);
    EXPECT_FALSE(unloadContentCalled);
    EXPECT_FALSE(upsampleChildrenCalled);
    EXPECT_FALSE(tile.content.renderContent.isMeshReady());
    EXPECT_TRUE(commands.empty());
}

TEST(TileRenderCommandPreparerTest, RunsSynchronousMeshPrepBeforeDrawableCheck) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    FrameResourceBudget budget;
    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    bool ensureMeshCalled = false;
    bool unloadContentCalled = false;
    bool upsampleChildrenCalled = false;

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        budget,
        makeContext(true),
        [&ensureMeshCalled](TilesetTile& meshTile) {
            ensureMeshCalled = true;
            meshTile.markRenderContentDone();
            meshTile.content.renderContent.setSurfaceMesh(
                std::make_unique<SurfaceTileMesh>());
            meshTile.content.renderContent.setMeshReady(true);
            meshTile.content.renderContent.setSurfaceGpuBuffers(
                std::make_unique<DummyBuffer>(4),
                nullptr);
        },
        [&unloadContentCalled](TilesetTile&) {
            unloadContentCalled = true;
        },
        [&upsampleChildrenCalled](TilesetTile&) {
            upsampleChildrenCalled = true;
        });

    EXPECT_TRUE(ensureMeshCalled);
    EXPECT_FALSE(unloadContentCalled);
    EXPECT_FALSE(upsampleChildrenCalled);
    EXPECT_TRUE(tile.content.renderContent.isMeshReady());
    EXPECT_TRUE(tile.hasSurfaceDrawable());
    EXPECT_TRUE(commands.empty());
}

TEST(TileRenderCommandPreparerTest,
     ContentProviderTerrainWithoutGltfDoesNotEnterLegacySurfacePrep) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.contentProviderTerrainQuadtreeTile = true;
    tile.content.contentKind = TileContentKind::Render;
    tile.content.loadState = TileLoadState::Done;
    tile.content.renderContent.setSurfaceMesh(
        std::make_unique<SurfaceTileMesh>());
    tile.content.renderContent.setMeshReady(true);
    tile.content.renderContent.setSurfaceSource(
        SurfaceDrawableSource::HeightmapTerrain);
    tile.content.renderContent.setSurfaceGpuBuffers(
        std::make_unique<DummyBuffer>(4),
        nullptr);
    ASSERT_TRUE(tile.content.renderContent.isMeshReady());
    ASSERT_TRUE(tile.hasSurfaceDrawable());
    tile.selectionFrameState.completeRenderable = true;
    tile.selectionFrameState.renderable = true;

    FrameResourceBudget budget;
    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    bool ensureMeshCalled = false;
    bool unloadContentCalled = false;
    bool upsampleChildrenCalled = false;

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        budget,
        makeContext(true),
        [&ensureMeshCalled](TilesetTile& meshTile) {
            ensureMeshCalled = true;
            meshTile.content.renderContent.clearSurfaceMeshResources();
            meshTile.content.loadState = TileLoadState::Unloaded;
        },
        [&unloadContentCalled](TilesetTile&) {
            unloadContentCalled = true;
        },
        [&upsampleChildrenCalled](TilesetTile&) {
            upsampleChildrenCalled = true;
        });

    EXPECT_FALSE(ensureMeshCalled);
    EXPECT_FALSE(unloadContentCalled);
    EXPECT_FALSE(upsampleChildrenCalled);
    EXPECT_TRUE(tile.content.renderContent.hasSurfaceMesh());
    EXPECT_TRUE(tile.content.renderContent.isMeshReady());
    EXPECT_FALSE(tile.renderableSnapshot(true).meshReady);
    EXPECT_FALSE(tile.selectionFrameState.completeRenderable);
    EXPECT_FALSE(tile.selectionFrameState.renderable);
    EXPECT_TRUE(commands.empty());
}

TEST(TileRenderCommandPreparerTest,
     GltfTerrainAncestorFallbackCarriesClipToPrimitiveCommand) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.renderContent.setGltfContent(std::make_unique<GltfModel>());
    tile.content.renderContent.setTerrainRenderContent(true);
    GltfPrimitiveRenderResources primitive;
    primitive.vertexBuffer = std::make_unique<DummyBuffer>(96);
    primitive.indexBuffer = std::make_unique<DummyBuffer>(12);
    primitive.vertexCount = 3;
    primitive.indexCount = 3;
    tile.content.renderContent.addGltfPrimitiveResource(std::move(primitive));
    tile.content.renderContent.setGltfResourcesReady(true);
    ASSERT_TRUE(tile.content.renderContent.isGltfRenderReady());

    FrameResourceBudget budget;
    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    bool ensureMeshCalled = false;
    bool unloadContentCalled = false;
    bool upsampleChildrenCalled = false;

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        budget,
        makeContext(true),
        [&ensureMeshCalled](TilesetTile&) {
            ensureMeshCalled = true;
        },
        [&unloadContentCalled](TilesetTile&) {
            unloadContentCalled = true;
        },
        [&upsampleChildrenCalled](TilesetTile&) {
            upsampleChildrenCalled = true;
        });

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_FALSE(ensureMeshCalled);
    EXPECT_FALSE(unloadContentCalled);
    EXPECT_FALSE(upsampleChildrenCalled);
    const RenderCommand& command = commands.front();
    EXPECT_EQ(RenderCommandKind::GltfPrimitive, command.kind);
    EXPECT_TRUE(command.terrainRenderContent);
    EXPECT_GT(command.surfaceClipEnabled, 0.5f);
    EXPECT_EQ(command.surfaceClipUv,
              (std::array<float, 4>{0.25f, 0.0f, 0.5f, 1.0f}));
    ASSERT_TRUE(command.uniforms.count("u_clipUV") > 0);
    ASSERT_TRUE(command.uniforms.count("u_clipEnabled") > 0);
    EXPECT_EQ(command.uniforms.at("u_clipUV"),
              (std::vector<float>{0.25f, 0.0f, 0.5f, 1.0f}));
    EXPECT_EQ(command.uniforms.at("u_clipEnabled"),
              (std::vector<float>{1.0f}));
}
