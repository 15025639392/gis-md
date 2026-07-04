#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileRenderCommandPreparer.h"

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/SurfaceTile.h"

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

std::unique_ptr<GltfModel> makeQuadTerrainGltfModel(
    const Rectangle& rectangle) {
    auto model = std::make_unique<GltfModel>();
    const Vec3 nodeOrigin(100.0, 200.0, 300.0);
    GltfNodeRuntime rootNode;
    rootNode.baseLocalTransform = Mat4::translation(nodeOrigin);
    rootNode.localTransform = rootNode.baseLocalTransform;
    rootNode.globalTransform = rootNode.baseLocalTransform;
    rootNode.mesh = 0;
    rootNode.hasMatrix = true;
    rootNode.baseTranslation = {nodeOrigin.x(), nodeOrigin.y(), nodeOrigin.z()};
    rootNode.translation = rootNode.baseTranslation;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);
    GltfPrimitive primitive;
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = nodeOrigin + Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = nodeOrigin + Vec3(2.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = nodeOrigin + Vec3(0.0, 2.0, 0.0);
    primitive.vertices[3].positionEcef = nodeOrigin + Vec3(2.0, 2.0, 0.0);
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
    primitive.runtime.baseVertices = primitive.vertices;
    for (SurfaceVertex& vertex : primitive.runtime.baseVertices) {
        vertex.positionEcef = vertex.positionEcef - nodeOrigin;
    }
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.hasNormals = true;
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
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
            auto gltfModel = makeQuadTerrainGltfModel(meshTile.bounds);
            meshTile.content.renderContent.prepareGltfContent(std::move(gltfModel), Mat4::identity());
            meshTile.content.renderContent.setTerrainRenderContent(true);
            GltfPrimitiveRenderResources resources;
            resources.vertexBuffer = std::make_unique<DummyBuffer>(64);
            resources.indexBuffer = std::make_unique<DummyBuffer>(12);
            resources.indexCount = 6;
            resources.vertexCount = 4;
            meshTile.content.renderContent.addGltfPrimitiveResource(std::move(resources));
            meshTile.content.renderContent.markRenderContentReady();
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
    EXPECT_TRUE(tile.content.renderContent.isGltfRenderReady());
    EXPECT_FALSE(commands.empty());
}

TEST(TileRenderCommandPreparerTest,
     ContentProviderTerrainWithoutGltfDoesNotEnterLegacySurfacePrep) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.contentKind = TileContentKind::Render;
    tile.content.loadState = TileLoadState::Done;
    auto gltfModel = makeQuadTerrainGltfModel(tile.bounds);
    tile.content.renderContent.prepareGltfContent(std::move(gltfModel), Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    GltfPrimitiveRenderResources resources;
    resources.vertexBuffer = std::make_unique<DummyBuffer>(64);
    resources.indexBuffer = std::make_unique<DummyBuffer>(12);
    resources.indexCount = 6;
    resources.vertexCount = 4;
    tile.content.renderContent.addGltfPrimitiveResource(std::move(resources));
    tile.content.renderContent.markRenderContentReady();
    ASSERT_TRUE(tile.content.renderContent.isGltfRenderReady());
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
            meshTile.content.renderContent.clearGltfContent();
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
    EXPECT_TRUE(tile.content.renderContent.hasGltfContent());
    EXPECT_TRUE(tile.content.renderContent.isGltfRenderReady());
    EXPECT_TRUE(tile.renderableSnapshot(true).meshReady);
    EXPECT_TRUE(tile.selectionFrameState.completeRenderable);
    EXPECT_TRUE(tile.selectionFrameState.renderable);
    ASSERT_EQ(commands.size(), 1u);
    EXPECT_EQ(commands.front().kind, RenderCommandKind::GltfPrimitive);
    EXPECT_TRUE(commands.front().terrainRenderContent);
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
    ASSERT_TRUE(command.hasGltfUniforms);
    EXPECT_EQ(command.gltfUniforms.clipUv,
              (std::array<float, 4>{0.25f, 0.0f, 0.5f, 1.0f}));
    EXPECT_FLOAT_EQ(1.0f, command.gltfUniforms.clipEnabled);
}

TEST(TileRenderCommandPreparerTest,
     OrdinaryGltfContentIgnoresTerrainFallbackClipUv) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.renderContent.setGltfContent(std::make_unique<GltfModel>());
    tile.content.renderContent.setTerrainRenderContent(false);
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

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        budget,
        makeContext(true),
        [](TilesetTile&) {
            FAIL() << "ordinary glTF render content must not enter surface prep";
        },
        [](TilesetTile&) {
            FAIL() << "ordinary glTF render content should not unload";
        },
        [](TilesetTile&) {
            FAIL() << "ordinary glTF render content should not upsample raster children";
        });

    ASSERT_EQ(commands.size(), 1u);
    const RenderCommand& command = commands.front();
    EXPECT_EQ(RenderCommandKind::GltfPrimitive, command.kind);
    EXPECT_FALSE(command.terrainRenderContent);
    EXPECT_EQ(command.surfaceClipEnabled, 0.0f);
    ASSERT_TRUE(command.hasGltfUniforms);
    EXPECT_EQ(command.gltfUniforms.clipUv,
              (std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f}));
    EXPECT_FLOAT_EQ(0.0f, command.gltfUniforms.clipEnabled);
}
