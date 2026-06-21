#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/renderer/RenderCommand.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/Frustum.h"
#include "earth_engine/scene/SceneFrameDiagnostics.h"
#include "earth_engine/scene/SceneFrameStateBuilder.h"
#include "earth_engine/scene/Scene.h"
#include "earth_engine/tiling/TileCacheKey.h"
#include "earth_engine/tiling/TileRasterOverlayPrefetcher.h"
#include "earth_engine/tiling/TileRenderPlanFrameRefresher.h"
#include "earth_engine/tiling/TileSelectionPlanAppender.h"
#include "earth_engine/tiling/TileSelectionRasterOverlayPreparer.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static std::string terrainCacheKey(const TileKey& key) {
        return TileCacheKey::forTile(key);
    }

    static void putTerrainCache(
        Tileset& tileset,
        const TileKey& key,
        std::unique_ptr<DecodedHeightmap> heightmap) {
        tileset.contentLifecycle_.terrainCache()[terrainCacheKey(key)] =
            std::move(heightmap);
    }

    static TileOcclusionState checkOcclusion(
        const Tileset& tileset,
        const TilesetTile& tile) {
        return tileset.checkOcclusion(tile);
    }

    static void ensureTileMesh(Tileset& tileset, TilesetTile& tile) {
        tileset.meshPreparation_.ensureTileMesh(tile);
    }

    static void prefetchRasterOverlays(Tileset& tileset, TilesetTile& tile) {
        const std::vector<size_t> overlayOrder =
            TileSelectionRasterOverlayPreparer::processingOrder(
                tileset.rasterOverlays_);
        TileRasterOverlayPrefetcher::prefetch(
            tile,
            tileset.rasterOverlays_,
            overlayOrder,
            tileset.device_,
            tileset.options_.maximumScreenSpaceError,
            tileset.frameResourceBudget_);
    }

    static void setInteractionActiveForFrame(Tileset& tileset, bool active) {
        tileset.interactionActiveForFrame_ = active;
    }

    static void beginTilePlan(Tileset& tileset) {
        tileset.tilePlan_ = TilePlan{};
    }

    static void addTileToCurrentPlan(Tileset& tileset, TilesetTile& tile) {
        TileSelectionPlanAppender::addTileToCurrentPlan(
            tileset.tilePlan_,
            tileset.loadQueue_,
            tileset.options_.enableLodTransitionPeriod,
            tile,
            1.0,
            true,
            std::numeric_limits<double>::max());
        TileRenderPlanFrameRefresher::refresh(
            tileset.tilePlan_,
            tileset.contentAccess_,
            tileset.rasterOverlays_,
            TileRenderPlanFrameRefreshOptions{
                tileset.options_.enableLodTransitionPeriod,
                tileset.interactionActiveForFrame_,
                tileset.resourceSmoothingActiveForFrame_});
    }
};
} // namespace earth_engine

namespace {

class DummyBuffer final : public Buffer {
public:
    explicit DummyBuffer(size_t byteSize) : byteSize_(byteSize) {}
    size_t size() const override { return byteSize_; }

private:
    size_t byteSize_ = 0;
};

class DummyShaderProgram final : public ShaderProgram {};

class DummyTexture final : public Texture {
public:
    DummyTexture(int width, int height) : width_(width), height_(height) {}
    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    int width_ = 0;
    int height_ = 0;
};

class DummyRenderDevice final : public RenderDevice {
public:
    Backend backendType() const override { return Backend::OpenGLES; }
    int maxTextureSize() const override { return 4096; }
    int maxDrawBuffers() const override { return 4; }
    bool supportsFloatTextures() const override { return true; }
    bool supportsInstancing() const override { return true; }
    std::string rendererString() const override { return "DummyRenderDevice"; }

    std::unique_ptr<Texture> createTexture(const TextureDesc& desc) override {
        return std::make_unique<DummyTexture>(desc.width, desc.height);
    }

    bool updateTextureRegion(
        Texture*,
        int,
        int,
        int,
        int,
        const uint8_t*,
        size_t) override {
        return false;
    }

    std::unique_ptr<Buffer> createBuffer(const BufferDesc& desc) override {
        return std::make_unique<DummyBuffer>(desc.size);
    }

    bool updateBuffer(Buffer*, size_t, const void*, size_t) override {
        return false;
    }

    std::unique_ptr<ShaderProgram> createShader(const ShaderDesc&) override {
        return std::make_unique<DummyShaderProgram>();
    }

    std::unique_ptr<Framebuffer> createFramebuffer(
        const FramebufferDesc&) override {
        return nullptr;
    }

    void beginFrame() override {}
    void submit(const RenderCommandList& commands) override {
        submittedCommands = commands;
    }
    void endFrame() override {}
    void onSurfaceCreated() override {}
    void onSurfaceChanged(int, int) override {}
    void onSurfaceDestroyed() override {}

    RenderCommandList submittedCommands;
};

SelectorView makeSelectorView(
    const Camera& camera,
    int viewportWidth,
    int viewportHeight) {
    SelectorView view;
    view.position = camera.position();
    view.direction = camera.direction();
    const double width = static_cast<double>(viewportWidth);
    const double height = static_cast<double>(viewportHeight);
    view.projectionMatrix = camera.projectionMatrix(width, height);
    view.frustum = Frustum::fromViewProjection(
        view.projectionMatrix * camera.viewMatrix());
    view.viewportHeightPixels = viewportHeight;
    return view;
}

std::unique_ptr<DecodedHeightmap> makeFlatHeightmap(float heightMeters) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {
        heightMeters,
        heightMeters,
        heightMeters,
        heightMeters};
    heightmap->minHeight = heightMeters;
    heightmap->maxHeight = heightMeters;
    return heightmap;
}

RasterOverlay::Options makeRasterOverlayOptions() {
    RasterOverlay::Options options{};
    options.maximumSimultaneousTileLoads = 20;
    options.maximumScreenSpaceError = 2.0;
    options.minimumZoom = 0;
    options.maximumZoom = 0;
    options.visible = true;
    options.opacity = 1.0f;
    options.role = RasterOverlayRole::BaseImagery;
    return options;
}

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
    primitive.indices = {0, 1, 2};
    model->primitives.push_back(std::move(primitive));
    return model;
}

GltfPrimitive makeTransparentTrianglePrimitiveAt(const Vec3& center) {
    GltfPrimitive primitive;
    primitive.vertices.resize(3);
    primitive.vertices[0].positionEcef =
        center + Vec3(-1.0, -1.0, 0.0);
    primitive.vertices[1].positionEcef =
        center + Vec3(2.0, -1.0, 0.0);
    primitive.vertices[2].positionEcef =
        center + Vec3(-1.0, 2.0, 0.0);
    for (SurfaceVertex& vertex : primitive.vertices) {
        vertex.normalEcef = Vec3::unitZ();
    }
    primitive.vertices[0].uv = {0.0f, 0.0f};
    primitive.vertices[1].uv = {1.0f, 0.0f};
    primitive.vertices[2].uv = {0.0f, 1.0f};
    primitive.vertexTexCoords[0] = {
        std::array<float, 2>{0.0f, 0.0f},
        std::array<float, 2>{1.0f, 0.0f},
        std::array<float, 2>{0.0f, 1.0f}};
    primitive.indices = {0, 1, 2};
    primitive.alphaMode = GltfAlphaMode::Blend;
    primitive.baseColorFactor = {1.0f, 1.0f, 1.0f, 0.5f};
    return primitive;
}

} // namespace

TEST(SceneFrameStateTest, SelectorViewOverrideFeedsMultipleViews) {
    Scene scene;
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());
    scene.update(1.0 / 60.0);
    EXPECT_EQ(scene.frameState().selectorViews.size(), 1u);

    Camera secondCamera;
    secondCamera.lookAt(
        target + Vec3(0.0, 1000000.0, 0.0),
        target,
        Vec3::unitZ());
    scene.setSelectorViewOverride({
        makeSelectorView(scene.camera(), 800, 600),
        makeSelectorView(secondCamera, 800, 600)});
    scene.update(1.0 / 60.0);
    EXPECT_EQ(scene.frameState().selectorViews.size(), 2u);

    scene.setSelectorViewOverride({});
    scene.update(1.0 / 60.0);
    EXPECT_TRUE(scene.frameState().selectorViews.empty());

    scene.clearSelectorViewOverride();
    scene.update(1.0 / 60.0);
    EXPECT_EQ(scene.frameState().selectorViews.size(), 1u);
}

TEST(SceneFrameStateTest, FrameDiagnosticsResetSmoothsAndRecordsTiming) {
    Diagnostics diagnostics;
    diagnostics.fps = 30.0;
    diagnostics.frameTimeMs = 123.0;
    diagnostics.cameraUpdateMs = 12.0;
    diagnostics.environmentUpdateMs = 13.0;
    diagnostics.basemapStackUpdateMs = 14.0;
    diagnostics.terrainUpdateMs = 15.0;
    diagnostics.contentTilesetUpdateMs = 16.0;
    diagnostics.renderCommandBuildMs = 17.0;
    diagnostics.renderSubmitMs = 18.0;
    diagnostics.drawCalls = 9;

    SceneFrameDiagnostics::resetPerFrame(diagnostics);
    EXPECT_EQ(diagnostics.cameraUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.environmentUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.basemapStackUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.terrainUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.contentTilesetUpdateMs, 0.0);
    EXPECT_EQ(diagnostics.renderCommandBuildMs, 0.0);
    EXPECT_EQ(diagnostics.renderSubmitMs, 0.0);
    EXPECT_EQ(diagnostics.drawCalls, 9);

    SceneFrameDiagnostics::updateFrameRate(diagnostics, 0.5);
    EXPECT_NEAR(diagnostics.frameTimeMs, 500.0, 1e-9);
    EXPECT_NEAR(diagnostics.fps, 27.2, 1e-9);

    SceneFrameDiagnostics::updateFrameRate(diagnostics, 0.0);
    EXPECT_NEAR(diagnostics.frameTimeMs, 500.0, 1e-9);
    EXPECT_NEAR(diagnostics.fps, 27.2, 1e-9);

    SceneFrameDiagnostics::recordEngineTiming(
        diagnostics,
        SceneFrameDiagnostics::EngineTimingScope::BeginFrame,
        1.25);
    SceneFrameDiagnostics::recordEngineTiming(
        diagnostics,
        SceneFrameDiagnostics::EngineTimingScope::SceneUpdate,
        2.5);
    SceneFrameDiagnostics::recordEngineTiming(
        diagnostics,
        SceneFrameDiagnostics::EngineTimingScope::SceneRender,
        3.75);
    SceneFrameDiagnostics::recordEngineTiming(
        diagnostics,
        SceneFrameDiagnostics::EngineTimingScope::EndFrame,
        4.0);
    SceneFrameDiagnostics::finishEngineFrame(diagnostics, 11.5);
    EXPECT_NEAR(diagnostics.engineBeginFrameMs, 1.25, 1e-9);
    EXPECT_NEAR(diagnostics.sceneUpdateMs, 2.5, 1e-9);
    EXPECT_NEAR(diagnostics.sceneRenderMs, 3.75, 1e-9);
    EXPECT_NEAR(diagnostics.engineEndFrameMs, 4.0, 1e-9);
    EXPECT_NEAR(diagnostics.engineFrameCpuMs, 11.5, 1e-9);
}

TEST(SceneFrameStateTest, FrameStateBuilderPopulatesPerFrameState) {
    Camera camera;
    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    camera.lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    FrameState frameState;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 600;
    SceneFrameStateBuildResult buildResult =
        SceneFrameStateBuilder::build(SceneFrameStateBuildInput{
            frameState,
            &camera,
            42,
            10.0,
            0.5,
            false,
            nullptr,
            true,
            Vec3::unitX(),
            8.0,
            nullptr,
            nullptr});

    EXPECT_EQ(frameState.frameId, 42u);
    EXPECT_NEAR(frameState.timeSeconds, 10.0, 1e-9);
    EXPECT_NEAR(frameState.deltaSeconds, 0.5, 1e-9);
    EXPECT_EQ(frameState.camera, &camera);
    ASSERT_EQ(frameState.selectorViews.size(), 1u);
    EXPECT_EQ(frameState.selectorViews.front().viewportHeightPixels, 600);
    EXPECT_TRUE(frameState.hasInteractionFocus);
    EXPECT_EQ(frameState.interactionFocusDirection, Vec3::unitX());
    EXPECT_EQ(buildResult.environmentUpdateMs, 0.0);

    std::vector<SelectorView> overrideViews{
        makeSelectorView(camera, 320, 240),
        makeSelectorView(camera, 640, 480)};
    SceneFrameStateBuilder::build(SceneFrameStateBuildInput{
        frameState,
        &camera,
        43,
        12.6,
        0.0,
        true,
        &overrideViews,
        true,
        Vec3::unitY(),
        10.0,
        nullptr,
        nullptr});

    ASSERT_EQ(frameState.selectorViews.size(), 2u);
    EXPECT_EQ(frameState.selectorViews[0].viewportHeightPixels, 240);
    EXPECT_EQ(frameState.selectorViews[1].viewportHeightPixels, 480);
    EXPECT_FALSE(frameState.hasInteractionFocus);
    EXPECT_EQ(frameState.interactionFocusDirection, Vec3::zero());

    SceneFrameStateBuilder::build(SceneFrameStateBuildInput{
        frameState,
        &camera,
        44,
        12.7,
        0.0,
        true,
        nullptr,
        false,
        Vec3::unitZ(),
        -1.0,
        nullptr,
        nullptr});

    EXPECT_TRUE(frameState.selectorViews.empty());
}

TEST(SceneFrameStateTest, OcclusionCallbackFeedsPrimaryAndAdditionalTilesets) {
    Scene scene;
    scene.setOcclusionCallback(
        [](const TilesetTile&) { return TileOcclusionState::Occluded; });

    auto makeTileset = []() {
        return std::make_unique<Tileset>(
            std::unique_ptr<TerrainProvider>{},
            TileScheme::createGeographicTMS(),
            std::vector<ActivatedRasterOverlay*>{},
            nullptr,
            TilesetOptions{});
    };

    auto primaryTileset = makeTileset();
    Tileset* primaryRaw = primaryTileset.get();
    scene.setTileset(std::move(primaryTileset));

    auto additionalTileset = makeTileset();
    Tileset* additionalRaw = additionalTileset.get();
    scene.addTileset(std::move(additionalTileset));

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* primaryRoot =
        TilesetTestAccess::ensureTile(*primaryRaw, rootKey);
    TilesetTile* additionalRoot =
        TilesetTestAccess::ensureTile(*additionalRaw, rootKey);
    ASSERT_NE(primaryRoot, nullptr);
    ASSERT_NE(additionalRoot, nullptr);

    EXPECT_EQ(
        TilesetTestAccess::checkOcclusion(*primaryRaw, *primaryRoot),
        TileOcclusionState::Occluded);
    EXPECT_EQ(
        TilesetTestAccess::checkOcclusion(*additionalRaw, *additionalRoot),
        TileOcclusionState::Occluded);

    scene.setOcclusionCallback(
        [](const TilesetTile&) { return TileOcclusionState::NotOccluded; });
    EXPECT_EQ(
        TilesetTestAccess::checkOcclusion(*primaryRaw, *primaryRoot),
        TileOcclusionState::NotOccluded);
    EXPECT_EQ(
        TilesetTestAccess::checkOcclusion(*additionalRaw, *additionalRoot),
        TileOcclusionState::NotOccluded);
}

TEST(
    SceneFrameStateTest,
    AdditionalTilesetDoesNotReplacePrimaryTerrainSampling) {
    Scene scene;
    scene.setViewport(800, 600, 1.0f);

    const TileKey westRoot{"Geographic-TMS", 0, 0, 0};
    const TileKey eastRoot{"Geographic-TMS", 0, 1, 0};

    auto terrainTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        nullptr,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();
    TilesetTestAccess::ensureTile(*terrainRaw, westRoot);
    TilesetTestAccess::ensureTile(*terrainRaw, eastRoot);
    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        westRoot,
        makeFlatHeightmap(123.0f));
    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        eastRoot,
        makeFlatHeightmap(123.0f));

    scene.setTileset(std::move(terrainTileset));
    ASSERT_EQ(scene.tileset(), terrainRaw);
    EXPECT_NEAR(scene.tileset()->sampleHeight(0.0, 0.0), 123.0f, 1e-6f);

    auto contentTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        nullptr,
        TilesetOptions{});
    TilesetTestAccess::ensureTile(*contentTileset, westRoot);
    TilesetTestAccess::ensureTile(*contentTileset, eastRoot);

    scene.addTileset(std::move(contentTileset));

    EXPECT_EQ(scene.tileset(), terrainRaw);
    EXPECT_EQ(scene.additionalTilesetCount(), 1u);
    EXPECT_NEAR(scene.tileset()->sampleHeight(0.0, 0.0), 123.0f, 1e-6f);
}

TEST(SceneFrameStateTest, AdditionalTilesetRendersGltfContent) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    auto terrainTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();
    TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        rootKey,
        makeFlatHeightmap(123.0f));
    scene.setTileset(std::move(terrainTileset));

    auto contentTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    TilesetTile* contentRoot =
        TilesetTestAccess::ensureTile(*contentTileset, rootKey);
    ASSERT_NE(contentRoot, nullptr);
    contentRoot->content.renderContent.setGltfContent(
        makeTriangleGltfModel());
    contentRoot->content.loadState = TileLoadState::Done;
    contentRoot->content.contentKind = TileContentKind::Render;

    scene.addTileset(std::move(contentTileset));
    scene.update(1.0 / 60.0);
    scene.render();

    const bool submittedGltf = std::any_of(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive;
        });
    EXPECT_TRUE(submittedGltf);
    EXPECT_GT(scene.diagnostics().renderGltfPrimitives, 0);
    EXPECT_EQ(scene.diagnostics().contentTilesets, 1);
    EXPECT_GT(scene.diagnostics().contentVisibleTiles, 0);
    EXPECT_EQ(scene.tileset(), terrainRaw);
    EXPECT_NEAR(scene.tileset()->sampleHeight(0.0, 0.0), 123.0f, 1e-6f);
}

TEST(SceneFrameStateTest, DiagnosticsExposeTerrainRenderEntryFallbackReasons) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    auto baseOverlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        makeRasterOverlayOptions());
    ActivatedRasterOverlay baseActivated(*baseOverlay);
    auto terrainTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(*terrainRaw, childKey);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);

    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(*terrainRaw, *root);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    RasterMappedToTilesetTile* rootMapped =
        root->rasterOverlayState.mappings().empty()
            ? nullptr
            : root->rasterOverlayState.mappings()[0].get();
    RasterOverlayTile* rootRaster =
        rootMapped ? rootMapped->getLoadingTile() : nullptr;
    ASSERT_NE(rootRaster, nullptr);
    rootRaster->setTexture(std::make_unique<DummyTexture>(4, 4));
    rootRaster->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);

    scene.setTileset(std::move(terrainTileset));
    scene.update(1.0 / 60.0);
    TilesetTestAccess::setInteractionActiveForFrame(*terrainRaw, true);
    TilesetTestAccess::beginTilePlan(*terrainRaw);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *child);
    scene.render();

    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesPlanned, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedPlanned, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesFadingPlanned, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesAncestorFallback, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSynchronousPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferredPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesFadingDrawn, 0);
    EXPECT_EQ(scene.diagnostics().terrainSurfaceCommandsSubmitted, 1);
    EXPECT_EQ(scene.diagnostics().globeFallbackCommands, 0);
    EXPECT_EQ(scene.diagnostics().globeFallbackMaskedTerrainEntries, 0);

    const auto surfaceCommandIt = std::find_if(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& submittedCommand) {
            return submittedCommand.kind == RenderCommandKind::SurfaceTile;
        });
    ASSERT_NE(device.submittedCommands.end(), surfaceCommandIt);
    const RenderCommand& command = *surfaceCommandIt;
    EXPECT_EQ(0, command.surfaceGeometryZoom);
    ASSERT_FALSE(command.textures.empty());
    EXPECT_EQ(rootRaster->getTexture(), command.textures.front());
    EXPECT_GT(command.surfaceClipEnabled, 0.5f);
    EXPECT_NEAR(0.0f, command.surfaceClipUv[0], 1e-6f);
    EXPECT_NEAR(0.5f, command.surfaceClipUv[1], 1e-6f);
    EXPECT_NEAR(0.5f, command.surfaceClipUv[2], 1e-6f);
    EXPECT_NEAR(0.5f, command.surfaceClipUv[3], 1e-6f);
}

TEST(SceneFrameStateTest, DiagnosticsExposeTerrainSynchronousPrepReasons) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    auto baseOverlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        makeRasterOverlayOptions());
    ActivatedRasterOverlay baseActivated(*baseOverlay);
    auto terrainTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    RasterMappedToTilesetTile* rootMapped =
        root->rasterOverlayState.mappings().empty()
            ? nullptr
            : root->rasterOverlayState.mappings()[0].get();
    RasterOverlayTile* rootRaster =
        rootMapped ? rootMapped->getLoadingTile() : nullptr;
    ASSERT_NE(rootRaster, nullptr);
    rootRaster->setTexture(std::make_unique<DummyTexture>(4, 4));
    rootRaster->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    ASSERT_FALSE(root->hasSurfaceDrawable());

    scene.setTileset(std::move(terrainTileset));
    scene.update(1.0 / 60.0);
    TilesetTestAccess::beginTilePlan(*terrainRaw);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *root);
    scene.render();

    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesPlanned, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedPlanned, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesAncestorFallback, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSynchronousPrep, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferredPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesMissed, 0);
    EXPECT_EQ(scene.diagnostics().terrainSurfaceCommandsSubmitted, 1);
    EXPECT_EQ(scene.diagnostics().globeFallbackCommands, 0);
    EXPECT_EQ(scene.diagnostics().globeFallbackMaskedTerrainEntries, 0);
}

TEST(SceneFrameStateTest, DiagnosticsRejectImageryOnlyAncestorFallback) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    scene.camera().lookAt(
        target + Vec3(1000000.0, 0.0, 0.0),
        target,
        Vec3::unitZ());

    auto baseOverlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        makeRasterOverlayOptions());
    ActivatedRasterOverlay baseActivated(*baseOverlay);
    auto terrainTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    TilesetTile* child = TilesetTestAccess::ensureTile(*terrainRaw, childKey);
    ASSERT_NE(root, nullptr);
    ASSERT_NE(child, nullptr);

    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    RasterMappedToTilesetTile* rootMapped =
        root->rasterOverlayState.mappings().empty()
            ? nullptr
            : root->rasterOverlayState.mappings()[0].get();
    RasterOverlayTile* rootRaster =
        rootMapped ? rootMapped->getLoadingTile() : nullptr;
    ASSERT_NE(rootRaster, nullptr);
    rootRaster->setTexture(std::make_unique<DummyTexture>(4, 4));
    rootRaster->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    ASSERT_FALSE(root->hasSurfaceDrawable());

    scene.setTileset(std::move(terrainTileset));
    scene.update(1.0 / 60.0);
    TilesetTestAccess::setInteractionActiveForFrame(*terrainRaw, true);
    TilesetTestAccess::beginTilePlan(*terrainRaw);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *child);
    scene.render();

    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesPlanned, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedPlanned, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesAncestorFallback, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSynchronousPrep, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferredPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesMissed, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferred, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedDeferred, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesFadingDeferred, 0);
    EXPECT_EQ(scene.diagnostics().terrainSurfaceCommandsSubmitted, 1);
    EXPECT_EQ(scene.diagnostics().globeFallbackCommands, 0);
    EXPECT_EQ(scene.diagnostics().globeFallbackMaskedTerrainEntries, 0);
}

TEST(SceneFrameStateTest, SortsTransparentGltfByCameraDepth) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(800, 600, 1.0f);

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 target(ellipsoid.semiMajorAxis(), 0.0, 0.0);
    const Vec3 cameraPosition = target + Vec3(1000000.0, 0.0, 0.0);
    scene.camera().lookAt(cameraPosition, target, Vec3::unitZ());

    auto contentTileset = std::make_unique<Tileset>(
        std::unique_ptr<TerrainProvider>{},
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root =
        TilesetTestAccess::ensureTile(*contentTileset, rootKey);
    ASSERT_NE(root, nullptr);

    auto model = std::make_unique<GltfModel>();
    model->primitives.push_back(
        makeTransparentTrianglePrimitiveAt(target + Vec3(900000.0, 0.0, 0.0)));
    model->primitives.push_back(makeTransparentTrianglePrimitiveAt(target));
    root->content.renderContent.setGltfContent(std::move(model));
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Render;
    scene.setTileset(std::move(contentTileset));

    scene.update(1.0 / 60.0);
    scene.render();

    std::vector<RenderCommand> transparentGltf;
    for (const RenderCommand& cmd : device.submittedCommands) {
        if (cmd.kind == RenderCommandKind::GltfPrimitive && cmd.blend) {
            transparentGltf.push_back(cmd);
        }
    }

    ASSERT_EQ(transparentGltf.size(), 2u);
    EXPECT_TRUE(transparentGltf[0].hasTranslucentSortDepth);
    EXPECT_TRUE(transparentGltf[1].hasTranslucentSortDepth);
    EXPECT_GT(
        transparentGltf[0].translucentSortDepth,
        transparentGltf[1].translucentSortDepth);
    EXPECT_LT(
        std::abs(transparentGltf[0].translucentSortDepth - 1000000.0),
        1.0);
    EXPECT_LT(
        std::abs(transparentGltf[1].translucentSortDepth - 100000.0),
        1.0);
}
