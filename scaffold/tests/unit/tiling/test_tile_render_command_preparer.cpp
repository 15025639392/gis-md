#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileRenderCommandPreparer.h"
#include "earth_engine/tiling/TileScheme.h"

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/GltfRenderGeometryBuilder.h"
#include "earth_engine/tiling/SurfaceTile.h"
#include "earth_engine/tiling/TileContentUploadPolicy.h"
#include "earth_engine/tiling/TileFillGeometrySignature.h"
#include "../../helpers/MockRenderDevice.h"

#include <array>
#include <cstring>
#include <memory>
#include <optional>
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

std::unique_ptr<GltfModel> makeAnimatedInstancedTriangleGltfModel() {
    auto model = std::make_unique<GltfModel>();
    GltfNodeRuntime rootNode;
    rootNode.mesh = 0;
    model->nodes.push_back(rootNode);
    model->sceneRootNodes.push_back(0);

    GltfPrimitive primitive;
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef = Vec3(0.0, 0.0, 0.0);
    primitive.vertices[1].positionEcef = Vec3(1.0, 0.0, 0.0);
    primitive.vertices[2].positionEcef = Vec3(0.0, 1.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.indices = {0, 1, 2};
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    primitive.runtime.hasNormals = true;
    primitive.instances.emplace_back();
    primitive.runtime.instanceTransforms.emplace_back();
    model->primitives.push_back(std::move(primitive));

    GltfAnimationSamplerRuntime sampler;
    sampler.inputTimes = {0.0, 1.0};
    sampler.outputValues = {
        0.0, 0.0, 0.0,
        5.0, 0.0, 0.0};
    sampler.outputComponents = 3;

    GltfAnimationChannelRuntime channel;
    channel.samplerIndex = 0;
    channel.targetNode = 0;
    channel.path = GltfAnimationPath::Translation;

    GltfAnimationRuntime animation;
    animation.samplers.push_back(std::move(sampler));
    animation.channels.push_back(channel);
    animation.durationSeconds = 1.0;
    model->animations.push_back(std::move(animation));
    model->setAnimationLooping(false);
    return model;
}

std::optional<Vec3> firstRenderedWorldPosition(
    const TilesetTile& tile) {
    const GltfPrimitiveRenderResources* resources =
        tile.content.renderContent.gltfPrimitiveResourceForReadAt(0);
    if (!resources) {
        return std::nullopt;
    }
    const auto* vertexBuffer =
        dynamic_cast<const earth_engine::testing::DummyBuffer*>(
            resources->vertexBuffer.get());
    const auto* instanceBuffer =
        dynamic_cast<const earth_engine::testing::DummyBuffer*>(
            resources->instanceBuffer.get());
    if (!vertexBuffer || !instanceBuffer ||
        vertexBuffer->bytes().size() < sizeof(GltfGpuVertex) ||
        instanceBuffer->bytes().size() < sizeof(GltfGpuInstance)) {
        return std::nullopt;
    }

    GltfGpuVertex vertex{};
    GltfGpuInstance instance{};
    std::memcpy(
        &vertex,
        vertexBuffer->bytes().data(),
        sizeof(vertex));
    std::memcpy(
        &instance,
        instanceBuffer->bytes().data(),
        sizeof(instance));
    const Vec3 instancePosition(
        static_cast<double>(
            instance.model[0] * vertex.pos[0] +
            instance.model[4] * vertex.pos[1] +
            instance.model[8] * vertex.pos[2] +
            instance.model[12]),
        static_cast<double>(
            instance.model[1] * vertex.pos[0] +
            instance.model[5] * vertex.pos[1] +
            instance.model[9] * vertex.pos[2] +
            instance.model[13]),
        static_cast<double>(
            instance.model[2] * vertex.pos[0] +
            instance.model[6] * vertex.pos[1] +
            instance.model[10] * vertex.pos[2] +
            instance.model[14]));
    return tile.content.renderContent.renderLocalOrigin() +
        instancePosition;
}

} // namespace

TEST(TileRenderCommandPreparerTest, DefersMeshPrepWhenSynchronousPrepDisabled) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    bool ensureMeshCalled = false;

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        makeContext(false),
        [&ensureMeshCalled](TilesetTile&) {
            ensureMeshCalled = true;
        });

    EXPECT_FALSE(ensureMeshCalled);
    EXPECT_FALSE(tile.content.renderContent.isMeshReady());
    EXPECT_TRUE(commands.empty());
}

TEST(TileRenderCommandPreparerTest, RunsSynchronousMeshPrepBeforeDrawableCheck) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    bool ensureMeshCalled = false;

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
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
        });

    EXPECT_TRUE(ensureMeshCalled);
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

    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    bool ensureMeshCalled = false;

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        makeContext(true),
        [&ensureMeshCalled](TilesetTile& meshTile) {
            ensureMeshCalled = true;
            meshTile.content.renderContent.clearGltfContent();
            meshTile.content.loadState = TileLoadState::Unloaded;
        });

    EXPECT_FALSE(ensureMeshCalled);
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

    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    bool ensureMeshCalled = false;

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        makeContext(true),
        [&ensureMeshCalled](TilesetTile&) {
            ensureMeshCalled = true;
        });

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_FALSE(ensureMeshCalled);
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
     GltfEllipsoidTerrainCommandKeepsFallbackSource) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    TileContentLoadResult result = TileContentLoadResult::renderTerrain(
        std::make_unique<GltfModel>(),
        TileLoadResultMetadata{},
        Mat4::identity(),
        TileTerrainRenderSource::EllipsoidFallback);
    TileContentUploadPolicy::prepareGltfRenderContent(
        tile,
        TileLoadedContent::fromContentResult(std::move(result)));
    GltfPrimitiveRenderResources primitive;
    primitive.vertexBuffer = std::make_unique<DummyBuffer>(96);
    primitive.indexBuffer = std::make_unique<DummyBuffer>(12);
    primitive.vertexCount = 3;
    primitive.indexCount = 3;
    tile.content.renderContent.addGltfPrimitiveResource(std::move(primitive));
    tile.content.renderContent.setGltfResourcesReady(true);

    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        makeContext(true),
        [](TilesetTile&) {});

    ASSERT_EQ(commands.size(), 1u);
    EXPECT_TRUE(commands.front().terrainRenderContent);
    EXPECT_EQ(TerrainSurfaceCommandSource::EllipsoidFallback,
              commands.front().terrainSurfaceSource);
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

    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        makeContext(true),
        [](TilesetTile&) {
            FAIL() << "ordinary glTF render content must not enter surface prep";
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

TEST(
    TileRenderCommandPreparerTest,
    StaticDrawDoesNotMutateRetainedResourceState) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    auto model = makeQuadTerrainGltfModel(tile.bounds);
    ASSERT_FALSE(model->primitives.empty());
    model->primitives.front().terrainGpuVertexBytes.resize(32);
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    GltfPrimitiveRenderResources resources;
    resources.vertexBuffer = std::make_unique<DummyBuffer>(64);
    resources.indexBuffer = std::make_unique<DummyBuffer>(12);
    resources.indexCount = 6;
    resources.vertexCount = 4;
    tile.content.renderContent.addGltfPrimitiveResource(
        std::move(resources));
    tile.markRenderContentDone();

    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    const uint64_t retainedRevisionBefore =
        tile.content.renderContent.retainedResourcesRevision();

    const bool resourcesChanged = TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        makeContext(true),
        [](TilesetTile&) {});

    EXPECT_FALSE(resourcesChanged);
    EXPECT_EQ(
        retainedRevisionBefore,
        tile.content.renderContent.retainedResourcesRevision());
    ASSERT_NE(nullptr, tile.content.renderContent.gltfModelForRead());
    EXPECT_FALSE(
        tile.content.renderContent.gltfModelForRead()
            ->primitives.front()
            .terrainGpuVertexBytes.empty());
    EXPECT_TRUE(tile.content.renderContent.isGltfRenderReady());
    EXPECT_EQ(1u, commands.size());
}

TEST(
    TileRenderCommandPreparerTest,
    UnreadyStaticGltfDoesNotPrepareGpuResourcesDuringDraw) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    auto model = makeQuadTerrainGltfModel(tile.bounds);
    ASSERT_FALSE(model->primitives.empty());
    model->primitives.front().hasTerrainWaterMaskMetadata = true;
    model->primitives.front().terrainGpuVertexBytes.resize(
        model->primitives.front().vertices.size() *
        sizeof(TerrainGpuVertex));
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.markRenderContentLoaded();

    earth_engine::testing::MockRenderDevice device;
    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    const uint64_t retainedRevisionBefore =
        tile.content.renderContent.retainedResourcesRevision();

    const bool resourcesChanged = TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        &device,
        makeContext(true),
        [](TilesetTile&) {});

    EXPECT_FALSE(resourcesChanged);
    EXPECT_EQ(0, device.createdBufferCount);
    EXPECT_TRUE(commands.empty());
    EXPECT_FALSE(tile.content.renderContent.isGltfRenderReady());
    EXPECT_EQ(TileLoadState::ContentLoaded, tile.content.loadState);
    EXPECT_EQ(
        retainedRevisionBefore,
        tile.content.renderContent.retainedResourcesRevision());
    ASSERT_NE(nullptr, tile.content.renderContent.gltfModelForRead());
    EXPECT_FALSE(
        tile.content.renderContent.gltfModelForRead()
            ->primitives.front()
            .terrainGpuVertexBytes.empty());
}

TEST(
    TileRenderCommandPreparerTest,
    AsyncTerrainUploadCommitsCompleteMetadataAndDrawOnlyConsumesIt) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    auto model = makeQuadTerrainGltfModel(tile.bounds);
    ASSERT_FALSE(model->primitives.empty());
    GltfPrimitive& sourcePrimitive = model->primitives.front();
    sourcePrimitive.hasTerrainWaterMaskMetadata = true;
    sourcePrimitive.terrainOnlyLand = false;
    sourcePrimitive.terrainOnlyWater = false;
    sourcePrimitive.terrainWaterMaskTextureIndex = 0u;
    sourcePrimitive.terrainWaterMaskTranslationX = 0.125;
    sourcePrimitive.terrainWaterMaskTranslationY = 0.25;
    sourcePrimitive.terrainWaterMaskScale = 0.5;
    sourcePrimitive.primitiveMode = GltfPrimitiveMode::TriangleStrip;
    sourcePrimitive.baseColorFactor = {0.25f, 0.5f, 0.75f, 1.0f};
    sourcePrimitive.terrainGpuVertexBytes.resize(
        sourcePrimitive.vertices.size() * sizeof(TerrainGpuVertex),
        7);
    GltfTexture waterMask;
    waterMask.image.width = 2;
    waterMask.image.height = 2;
    waterMask.image.channels = 1;
    waterMask.image.pixels = {0u, 64u, 192u, 255u};
    waterMask.sampler.minFilter = GltfTextureFilter::Nearest;
    waterMask.sampler.magFilter = GltfTextureFilter::Nearest;
    waterMask.sampler.mipmap = false;
    waterMask.sampler.wrapS = GltfTextureWrap::Repeat;
    waterMask.sampler.wrapT = GltfTextureWrap::MirroredRepeat;
    model->textures.push_back(std::move(waterMask));
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.markRenderContentLoaded();
    tile.content.renderContent.asyncGpuUploadPending = true;

    std::optional<GpuReadyData> ready =
        GltfRenderResourcePreparer::prepareCpuWork(tile, 1.25);
    ASSERT_TRUE(ready.has_value());
    ASSERT_EQ(1u, ready->primitives.size());
    EXPECT_EQ(4u, ready->primitives.front().vertexCount);
    EXPECT_EQ(6u, ready->primitives.front().indexCount);
    ASSERT_EQ(1u, ready->textures.size());
    ASSERT_TRUE(
        ready->primitives.front().terrainWaterMaskTextureIndex.has_value());
    EXPECT_EQ(
        0u,
        *ready->primitives.front().terrainWaterMaskTextureIndex);

    earth_engine::testing::MockRenderDevice device;
    ASSERT_TRUE(GltfRenderResourcePreparer::uploadToGpu(
        tile,
        &device,
        std::move(*ready)));
    ASSERT_EQ(2, device.createdBufferCount);
    ASSERT_EQ(1, device.createdTextureCount);
    EXPECT_EQ(2, device.lastTextureDesc.width);
    EXPECT_EQ(2, device.lastTextureDesc.height);
    EXPECT_EQ(TextureDesc::Format::R8, device.lastTextureDesc.format);
    EXPECT_FALSE(device.lastTextureDesc.mipmap);
    EXPECT_EQ(
        TextureDesc::Filter::Nearest,
        device.lastTextureDesc.minFilter);
    EXPECT_EQ(
        TextureDesc::Filter::Nearest,
        device.lastTextureDesc.magFilter);
    EXPECT_EQ(TextureDesc::Wrap::Repeat, device.lastTextureDesc.wrapS);
    EXPECT_EQ(
        TextureDesc::Wrap::MirroredRepeat,
        device.lastTextureDesc.wrapT);
    ASSERT_TRUE(tile.content.renderContent.isGltfRenderReady());
    ASSERT_EQ(TileLoadState::Done, tile.content.loadState);
    ASSERT_EQ(1u, tile.content.renderContent.gltfPrimitiveResourceCount());
    const GltfPrimitiveRenderResources* resources =
        tile.content.renderContent.gltfPrimitiveResourceForReadAt(0);
    ASSERT_NE(nullptr, resources);
    EXPECT_NE(nullptr, resources->vertexBuffer);
    EXPECT_NE(nullptr, resources->indexBuffer);
    EXPECT_EQ(4, resources->vertexCount);
    EXPECT_EQ(6, resources->indexCount);
    EXPECT_EQ(GltfPrimitiveMode::TriangleStrip, resources->primitiveMode);
    EXPECT_TRUE(resources->useTerrainVertexFormat);
    EXPECT_TRUE(resources->hasTerrainWaterMaskMetadata);
    EXPECT_FALSE(resources->terrainOnlyLand);
    EXPECT_FALSE(resources->terrainOnlyWater);
    EXPECT_NE(nullptr, resources->terrainWaterMaskTexture);
    EXPECT_EQ(
        (std::array<float, 4>{0.125f, 0.25f, 0.5f, 0.0f}),
        resources->terrainWaterMaskTranslationScale);
    EXPECT_EQ(
        (std::array<float, 4>{0.25f, 0.5f, 0.75f, 1.0f}),
        resources->baseColorFactor);

    const int bufferCountAfterUpload = device.createdBufferCount;
    Buffer* const uploadedVertexBuffer = resources->vertexBuffer.get();
    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    const bool resourcesChanged = TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        &device,
        makeContext(true),
        [](TilesetTile&) {});

    EXPECT_FALSE(resourcesChanged);
    EXPECT_EQ(bufferCountAfterUpload, device.createdBufferCount);
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(uploadedVertexBuffer, commands.front().vertexBuffer);
    EXPECT_GT(commands.front().gltfHasWaterMask, 0.5f);
    ASSERT_GT(
        commands.front().textures.size(),
        static_cast<size_t>(kGltfWaterMaskTextureSlot));
    EXPECT_EQ(
        resources->terrainWaterMaskTexture,
        commands.front().textures[kGltfWaterMaskTextureSlot]);
}

TEST(
    TileRenderCommandPreparerTest,
    CpuReadyUploadUsesOneComputedLocalOriginForGeometryAndTileState) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    const Mat4 contentTransform =
        Mat4::translation(Vec3(10.0, 20.0, 30.0));
    tile.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(tile.bounds),
        contentTransform);
    tile.markRenderContentLoaded();

    const GltfModel* model =
        tile.content.renderContent.gltfModelForRead();
    ASSERT_NE(nullptr, model);
    const Vec3 expectedLocalOrigin =
        GltfRenderGeometryBuilder::localOrigin(
            *model,
            contentTransform);

    std::optional<GpuReadyData> ready =
        GltfRenderResourcePreparer::prepareCpuWork(tile, 1.25);
    ASSERT_TRUE(ready.has_value());
    ASSERT_EQ(1u, ready->primitives.size());
    ASSERT_GE(
        ready->primitives.front().vertexBytes.size(),
        sizeof(GltfGpuVertex));
    GltfGpuVertex firstVertex{};
    std::memcpy(
        &firstVertex,
        ready->primitives.front().vertexBytes.data(),
        sizeof(firstVertex));
    EXPECT_NEAR(-1.0f, firstVertex.pos[0], 1e-6f);
    EXPECT_NEAR(-1.0f, firstVertex.pos[1], 1e-6f);
    EXPECT_NEAR(0.0f, firstVertex.pos[2], 1e-6f);

    earth_engine::testing::MockRenderDevice device;
    ASSERT_TRUE(GltfRenderResourcePreparer::uploadToGpu(
        tile,
        &device,
        std::move(*ready)));
    EXPECT_EQ(
        expectedLocalOrigin,
        tile.content.renderContent.renderLocalOrigin());
}

TEST(
    TileRenderCommandPreparerTest,
    AnimatedGpuInstancingUpdatesGeometryInstancesAndOriginTogether) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.renderContent.prepareGltfContent(
        makeAnimatedInstancedTriangleGltfModel(),
        Mat4::translation(Vec3(100.0, 0.0, 0.0)));
    tile.markRenderContentLoaded();

    earth_engine::testing::MockRenderDevice device;
    GltfRenderResourcePreparer::prepare(tile, &device, 0.0);

    ASSERT_TRUE(tile.content.renderContent.isGltfRenderReady());
    ASSERT_EQ(3, device.createdBufferCount);
    ASSERT_EQ(0, device.updatedBufferCount);
    const std::optional<Vec3> frameZeroPosition =
        firstRenderedWorldPosition(tile);
    ASSERT_TRUE(frameZeroPosition.has_value());
    EXPECT_NEAR(100.0, frameZeroPosition->x(), 1e-5);

    GltfRenderResourcePreparer::prepare(tile, nullptr, 1.0);
    EXPECT_EQ(0, device.updatedBufferCount);
    const std::optional<Vec3> staleGpuPosition =
        firstRenderedWorldPosition(tile);
    ASSERT_TRUE(staleGpuPosition.has_value());
    EXPECT_NEAR(100.0, staleGpuPosition->x(), 1e-5);

    GltfRenderResourcePreparer::prepare(tile, &device, 1.0);

    ASSERT_TRUE(tile.content.renderContent.isGltfRenderReady());
    EXPECT_EQ(3, device.createdBufferCount);
    EXPECT_EQ(2, device.updatedBufferCount);
    const std::optional<Vec3> frameOnePosition =
        firstRenderedWorldPosition(tile);
    ASSERT_TRUE(frameOnePosition.has_value());
    EXPECT_NEAR(105.0, frameOnePosition->x(), 1e-5);
    EXPECT_NEAR(
        5.0,
        frameOnePosition->x() - frameZeroPosition->x(),
        1e-5);
}

TEST(
    TileRenderCommandPreparerTest,
    SynchronousTerrainTextureFailureDoesNotCommitDoneOrDropReadyFill) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.renderContent.setFillContent(
        makeQuadTerrainGltfModel(tile.bounds));
    GltfPrimitiveRenderResources fillResources;
    fillResources.vertexBuffer = std::make_unique<DummyBuffer>(64);
    fillResources.indexBuffer = std::make_unique<DummyBuffer>(24);
    fillResources.vertexCount = 4;
    fillResources.indexCount = 6;
    tile.content.renderContent.beginFillGpuResourceBuild(0, 1);
    tile.content.renderContent.addFillPrimitiveResource(
        std::move(fillResources));
    tile.content.renderContent.commitFillResourcesReady(
        TileFillGeometrySignature{
            tile.bounds,
            RasterOverlayProjection::Geographic,
            1});

    auto model = makeQuadTerrainGltfModel(tile.bounds);
    GltfPrimitive& primitive = model->primitives.front();
    primitive.hasTerrainWaterMaskMetadata = true;
    primitive.terrainOnlyLand = false;
    primitive.terrainOnlyWater = false;
    primitive.terrainWaterMaskTextureIndex = 0u;
    GltfTexture waterMask;
    waterMask.image.width = 2;
    waterMask.image.height = 2;
    waterMask.image.channels = 1;
    waterMask.image.pixels = {0u, 64u, 192u, 255u};
    model->textures.push_back(std::move(waterMask));
    tile.content.renderContent.prepareGltfContent(
        std::move(model),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.markRenderContentLoaded();

    earth_engine::testing::MockRenderDevice device;
    device.allowTextureCreation = false;
    GltfRenderResourcePreparer::prepare(tile, &device, 1.25);

    EXPECT_EQ(TileLoadState::FailedTemporarily, tile.content.loadState);
    EXPECT_FALSE(tile.content.renderContent.isRenderContentReady());
    EXPECT_FALSE(tile.content.renderContent.hasGltfPrimitiveResources());
    EXPECT_TRUE(tile.content.renderContent.isFillReady());
    EXPECT_TRUE(tile.content.renderContent.drawsFill());
}

TEST(
    TileRenderCommandPreparerTest,
    StableDoneContentKeepsResidentDrawCommandsAcrossFrames) {
    TilesetTile tile(TileKey{"test", 0, 0, 0}, Rectangle{});
    tile.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(tile.bounds),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    GltfPrimitiveRenderResources resources;
    resources.vertexBuffer = std::make_unique<DummyBuffer>(64);
    resources.indexBuffer = std::make_unique<DummyBuffer>(12);
    resources.indexCount = 6;
    resources.vertexCount = 4;
    tile.content.renderContent.addGltfPrimitiveResource(
        std::move(resources));
    tile.markRenderContentDone();

    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;
    TileRenderCommandPerformanceTimings firstFrameTimings;

    const bool firstFrameResourcesChanged =
        TileRenderCommandPreparer::build(
            renderer,
            tile,
            commands,
            overlays,
            nullptr,
            makeContext(true),
            [](TilesetTile&) {},
            &firstFrameTimings);

    ASSERT_EQ(1u, commands.size());
    EXPECT_FALSE(firstFrameResourcesChanged);
    EXPECT_EQ(1, firstFrameTimings.drawCommand.cacheRebuildCount);
    EXPECT_TRUE(tile.content.renderContent.hasCachedDrawCommands());

    commands.clear();
    TileRenderCommandPerformanceTimings secondFrameTimings;
    TileRenderCommandPrepareContext secondFrameContext = makeContext(true);
    secondFrameContext.frameNumber += 1;
    const bool secondFrameResourcesChanged =
        TileRenderCommandPreparer::build(
            renderer,
            tile,
            commands,
            overlays,
            nullptr,
            secondFrameContext,
            [](TilesetTile&) {},
            &secondFrameTimings);

    ASSERT_EQ(1u, commands.size());
    EXPECT_FALSE(secondFrameResourcesChanged);
    EXPECT_EQ(0, secondFrameTimings.drawCommand.cacheRebuildCount);
    EXPECT_TRUE(tile.content.renderContent.hasCachedDrawCommands());
}

TEST(
    TileRenderCommandPreparerTest,
    PendingAsyncTerrainUploadDrawsFillWithoutSynchronousGpuPreparation) {
    TilesetTile tile(
        TileKey{"Geographic-TMS", 1, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 0.0, 0.0));
    tile.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(tile.bounds),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    tile.content.renderContent.asyncGpuUploadPending = true;

    GltfPrimitiveRenderResources fillResources;
    fillResources.vertexBuffer = std::make_unique<DummyBuffer>(64);
    fillResources.indexBuffer = std::make_unique<DummyBuffer>(12);
    fillResources.vertexCount = 4;
    fillResources.indexCount = 6;
    Buffer* const fillVertexBuffer = fillResources.vertexBuffer.get();
    tile.content.renderContent.beginFillGpuResourceBuild(0, 1);
    tile.content.renderContent.addFillPrimitiveResource(
        std::move(fillResources));
    tile.content.renderContent.commitFillResourcesReady(
        TileFillGeometrySignature{
            tile.bounds,
            RasterOverlayProjection::Geographic,
            1});

    earth_engine::testing::MockRenderDevice device;
    std::vector<ActivatedRasterOverlay*> overlays;
    Renderer renderer(nullptr);
    RenderCommandList commands;

    const bool resourcesChanged = TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        &device,
        makeContext(true),
        [](TilesetTile&) {});

    EXPECT_FALSE(resourcesChanged);
    EXPECT_EQ(0, device.createdBufferCount);
    EXPECT_TRUE(tile.content.renderContent.asyncGpuUploadPending);
    EXPECT_FALSE(tile.content.renderContent.isRenderContentReady());
    EXPECT_TRUE(tile.content.renderContent.isFillReady());
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ(fillVertexBuffer, commands.front().vertexBuffer);
    EXPECT_EQ(
        TerrainSurfaceCommandSource::FillProxy,
        commands.front().terrainSurfaceSource);
}

TEST(
    TileRenderCommandPreparerTest,
    DrawBuildDoesNotAdvanceUnpreparedRasterOverlayState) {
    RasterOverlay::Options options;
    options.blocksCompleteRenderable = false;
    auto overlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        options);
    ActivatedRasterOverlay activated(*overlay);
    std::vector<ActivatedRasterOverlay*> overlays{&activated};

    TilesetTile tile(
        TileKey{"Geographic-TMS", 0, 0, 0},
        Rectangle::fromDegrees(-180.0, -90.0, 0.0, 90.0));
    tile.content.renderContent.prepareGltfContent(
        makeQuadTerrainGltfModel(tile.bounds),
        Mat4::identity());
    tile.content.renderContent.setTerrainRenderContent(true);
    GltfPrimitiveRenderResources resources;
    resources.vertexBuffer = std::make_unique<DummyBuffer>(64);
    resources.indexBuffer = std::make_unique<DummyBuffer>(12);
    resources.indexCount = 6;
    resources.vertexCount = 4;
    tile.content.renderContent.addGltfPrimitiveResource(
        std::move(resources));
    tile.markRenderContentDone();

    Renderer renderer(nullptr);
    RenderCommandList commands;
    const uint64_t updateCountBefore =
        tile.rasterOverlayState.authoritativeUpdateCount();

    TileRenderCommandPreparer::build(
        renderer,
        tile,
        commands,
        overlays,
        nullptr,
        makeContext(true),
        [](TilesetTile&) {});

    EXPECT_EQ(
        updateCountBefore,
        tile.rasterOverlayState.authoritativeUpdateCount());
    EXPECT_EQ(0u, tile.rasterOverlayState.mappingCount());
    ASSERT_EQ(1u, commands.size());
}
