#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/layers/RasterOverlay.h"
#include "earth_engine/providers/DebugImageryProvider.h"
#include "earth_engine/providers/RasterOverlayTileProvider.h"
#include "earth_engine/renderer/RenderCommand.h"
#include "earth_engine/renderer/RenderDevice.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/Frustum.h"
#include "earth_engine/scene/PresentationTrace.h"
#include "earth_engine/scene/ScenePresentationTraceBuilder.h"
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
        tileset.contentLifecycle_
            .heightmapTerrainCache()[terrainCacheKey(key)] =
            std::move(heightmap);
    }

    static void setLoadedGltfTerrainContent(
        TilesetTile& tile,
        std::unique_ptr<GltfModel> model) {
        tile.content.renderContent.prepareGltfContent(
            std::move(model),
            Mat4::identity());
        tile.content.renderContent.setTerrainRenderContent(true);
        tile.markRenderContentDone();
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

    static TilePlan& mutableTilePlan(Tileset& tileset) {
        return tileset.tilePlan_;
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

Camera makeCameraFromCenterPitchHeading(
    double longitudeDegrees,
    double latitudeDegrees,
    double cameraHeightMeters,
    double pitchRadians,
    double headingRadians) {
    const Cartographic targetCartographic =
        Cartographic::fromDegrees(longitudeDegrees, latitudeDegrees, 0.0);
    const Vec3 target =
        Ellipsoid::WGS84().cartographicToCartesian(targetCartographic);
    const Vec3 localUp =
        Ellipsoid::WGS84().geodeticSurfaceNormal(targetCartographic);
    const Vec3 east = Vec3(-target.y(), target.x(), 0.0).normalized();
    const Vec3 north = localUp.cross(east).normalized();

    const double horizontalScale = std::cos(pitchRadians);
    const Vec3 direction =
        (east * (std::sin(headingRadians) * horizontalScale) +
         north * (std::cos(headingRadians) * horizontalScale) +
         localUp * std::sin(pitchRadians)).normalized();
    const Vec3 cameraUp =
        (north * std::cos(headingRadians) -
         east * std::sin(headingRadians)).normalized();

    Camera camera;
    camera.lookAt(target - direction * cameraHeightMeters, target, cameraUp);
    return camera;
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

std::unique_ptr<GltfModel> makeFlatGeographicTerrainGltfModel(
    const Rectangle& rectangle,
    double heightMeters) {
    auto model = std::make_unique<GltfModel>();
    GltfPrimitive primitive;
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    primitive.vertices.resize(4);
    primitive.vertices[0].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            rectangle.west(),
            rectangle.south(),
            heightMeters));
    primitive.vertices[1].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            rectangle.east(),
            rectangle.south(),
            heightMeters));
    primitive.vertices[2].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            rectangle.west(),
            rectangle.north(),
            heightMeters));
    primitive.vertices[3].positionEcef = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(
            rectangle.east(),
            rectangle.north(),
            heightMeters));
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.primitiveMode = GltfPrimitiveMode::Triangles;
    primitive.runtime.nodeIndex = 0;
    primitive.runtime.baseVertices = primitive.vertices;
    model->primitives.push_back(std::move(primitive));
    model->rasterOverlayDetails.setGeographicRectangle(rectangle);
    return model;
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

const std::vector<std::unique_ptr<Tileset>>& emptyContentTilesets() {
    static const std::vector<std::unique_ptr<Tileset>> empty;
    return empty;
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

TEST(SceneFrameStateTest, PresentationTraceRecordsDeterministicCameraState) {
    DummyRenderDevice device;
    Scene scene;
    ASSERT_TRUE(scene.setRenderDevice(&device));
    scene.setViewport(1024, 768, 2.0f);

    constexpr double kLongitudeDegrees = 116.3913;
    constexpr double kLatitudeDegrees = 39.9075;
    constexpr double kRangeMeters = 750000.0;
    const double pitch = Transforms::toRadians(-65.0);
    const double heading = Transforms::toRadians(35.0);
    scene.camera() = makeCameraFromCenterPitchHeading(
        kLongitudeDegrees,
        kLatitudeDegrees,
        kRangeMeters,
        pitch,
        heading);

    scene.update(1.0 / 60.0);
    scene.render();

    const PresentationTrace& trace = scene.presentationTrace();
    EXPECT_EQ(trace.camera.frameId, scene.frameState().frameId);
    EXPECT_EQ(trace.camera.viewportWidthPixels, 1024);
    EXPECT_EQ(trace.camera.viewportHeightPixels, 768);
    EXPECT_NEAR(trace.camera.devicePixelRatio, 2.0f, 1e-6f);
    EXPECT_NEAR(
        trace.camera.targetLongitudeDegrees,
        kLongitudeDegrees,
        1e-8);
    EXPECT_NEAR(
        trace.camera.targetLatitudeDegrees,
        kLatitudeDegrees,
        1e-8);
    EXPECT_NEAR(trace.camera.pitchRadians, pitch, 1e-10);
    EXPECT_NEAR(trace.camera.headingRadians, heading, 1e-10);
    ASSERT_EQ(trace.selectorViews.size(), 1u);
    EXPECT_EQ(trace.selectorViews.front().viewportHeightPixels, 768);
    EXPECT_FALSE(trace.commands.empty());
}

TEST(SceneFrameStateTest, OcclusionCallbackFeedsPrimaryAndAdditionalTilesets) {
    Scene scene;
    scene.setOcclusionCallback(
        [](const TilesetTile&) { return TileOcclusionState::Occluded; });

    auto makeTileset = []() {
        return std::make_unique<Tileset>(
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
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        nullptr,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();
    TilesetTile* westTile = TilesetTestAccess::ensureTile(
        *terrainRaw,
        westRoot);
    TilesetTile* eastTile = TilesetTestAccess::ensureTile(
        *terrainRaw,
        eastRoot);
    ASSERT_NE(westTile, nullptr);
    ASSERT_NE(eastTile, nullptr);
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *westTile,
        makeFlatGeographicTerrainGltfModel(westTile->bounds, 123.0));
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *eastTile,
        makeFlatGeographicTerrainGltfModel(eastTile->bounds, 123.0));

    scene.setTileset(std::move(terrainTileset));
    ASSERT_EQ(scene.tileset(), terrainRaw);
    EXPECT_NEAR(scene.tileset()->sampleHeight(0.0, 0.0), 123.0f, 1e-6f);

    auto contentTileset = std::make_unique<Tileset>(
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
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();
    TilesetTile* terrainRoot =
        TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    ASSERT_NE(terrainRoot, nullptr);
    TilesetTestAccess::setLoadedGltfTerrainContent(
        *terrainRoot,
        makeFlatGeographicTerrainGltfModel(terrainRoot->bounds, 123.0));
    scene.setTileset(std::move(terrainTileset));

    auto contentTileset = std::make_unique<Tileset>(
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
    const bool submittedTerrainGltf = std::any_of(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive &&
                   cmd.terrainRenderContent;
        });
    const bool submittedNonTerrainGltf = std::any_of(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive &&
                   !cmd.terrainRenderContent;
        });
    EXPECT_TRUE(submittedGltf);
    EXPECT_TRUE(submittedTerrainGltf);
    EXPECT_TRUE(submittedNonTerrainGltf);
    EXPECT_GT(scene.diagnostics().renderGltfPrimitives, 0);
    EXPECT_EQ(scene.diagnostics().terrainSurfaceMeshes, 2);
    EXPECT_GT(scene.diagnostics().terrainRenderContentCommands, 0);
    EXPECT_EQ(scene.diagnostics().contentTilesets, 1);
    EXPECT_GT(scene.diagnostics().contentVisibleTiles, 0);
    EXPECT_GT(scene.diagnostics().terrainRenderEntriesPlanned, 0);
    EXPECT_GT(scene.diagnostics().terrainRenderEntriesSelectedPlanned, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesFadingPlanned, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesAncestorFallback, 0);
    EXPECT_GE(scene.diagnostics().terrainRenderEntriesSynchronousPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferredPrep, 0);
    EXPECT_GT(scene.diagnostics().terrainRenderEntriesDrawn, 0);
    EXPECT_GT(scene.diagnostics().terrainRenderEntriesSelectedDrawn, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesFadingDrawn, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesMissed, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedMissed, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesFadingMissed, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferred, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedDeferred, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesFadingDeferred, 0);
    EXPECT_GT(scene.diagnostics().terrainSurfaceCommandsSubmitted, 0);
    EXPECT_EQ(scene.diagnostics().globeFallbackCommands, 0);
    EXPECT_EQ(scene.diagnostics().globeFallbackMaskedTerrainEntries, 0);
    EXPECT_EQ(scene.tileset(), terrainRaw);
    EXPECT_NEAR(scene.tileset()->sampleHeight(0.0, 0.0), 123.0f, 1e-6f);
}

TEST(SceneFrameStateTest, GltfTerrainCountsAsTerrainRenderContent) {
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
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    TilesetTile* root =
        TilesetTestAccess::ensureTile(*terrainTileset, rootKey);
    ASSERT_NE(root, nullptr);
    root->content.renderContent.setGltfContent(makeTriangleGltfModel());
    root->content.renderContent.setTerrainRenderContent(true);
    root->content.loadState = TileLoadState::Done;
    root->content.contentKind = TileContentKind::Render;
    scene.setTileset(std::move(terrainTileset));

    scene.update(1.0 / 60.0);
    scene.render();

    const int terrainGltfCommands = static_cast<int>(std::count_if(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& cmd) {
            return cmd.kind == RenderCommandKind::GltfPrimitive &&
                   cmd.terrainRenderContent;
        }));
    EXPECT_GT(terrainGltfCommands, 0);
    EXPECT_EQ(scene.diagnostics().renderGltfPrimitives, terrainGltfCommands);
    EXPECT_EQ(
        scene.diagnostics().terrainRenderContentCommands,
        terrainGltfCommands +
            scene.diagnostics().terrainSurfaceCommandsSubmitted);
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

    const PresentationTrace& trace = scene.presentationTrace();
    ASSERT_EQ(1u, trace.tilesets.size());
    const PresentationTilesetTrace& tilesetTrace = trace.tilesets.front();
    ASSERT_EQ(1u, tilesetTrace.renderEntries.size());
    const PresentationRenderEntryTrace& entryTrace =
        tilesetTrace.renderEntries.front();
    EXPECT_EQ(childKey, entryTrace.selectedKey);
    EXPECT_EQ(rootKey, entryTrace.renderKey);
    EXPECT_TRUE(entryTrace.usesAncestorFallback);
    EXPECT_TRUE(entryTrace.surfaceClipEnabled);
    EXPECT_EQ(command.surfaceClipUv, entryTrace.surfaceClipUv);
    EXPECT_EQ(1, tilesetTrace.renderEntryAncestorFallbackCount);
    EXPECT_EQ(0, tilesetTrace.renderEntrySynchronousPrepCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryDeferredPrepCount);
    EXPECT_EQ(1, tilesetTrace.renderEntryPlannedCommandCount);
    EXPECT_EQ(1, tilesetTrace.renderEntrySelectedPlannedCommandCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryFadingPlannedCommandCount);
    EXPECT_EQ(1, tilesetTrace.renderEntryCommandDrawCount);
    EXPECT_EQ(1, tilesetTrace.renderEntrySelectedCommandDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryFadingCommandDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryCommandMissedDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntrySelectedCommandMissedDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryFadingCommandMissedDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryCommandMissingSelectedCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryCommandMissingRenderCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryCommandDeferredCount);
    EXPECT_EQ(0, tilesetTrace.renderEntrySelectedCommandDeferredCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryFadingCommandDeferredCount);

    const auto commandTraceIt = std::find_if(
        trace.commands.begin(),
        trace.commands.end(),
        [](const PresentationCommandTrace& commandTrace) {
            return commandTrace.kind == RenderCommandKind::SurfaceTile;
        });
    ASSERT_NE(trace.commands.end(), commandTraceIt);
    EXPECT_EQ(command.surfaceClipUv, commandTraceIt->surfaceClipUv);
    EXPECT_NE(
        std::string::npos,
        commandTraceIt->stableKey.find("clip:Geographic-TMS/1/0/0"));
}

TEST(SceneFrameStateTest, RenderPlanKeepsSurfaceBeforeBaseRasterIsDrawable) {
    DummyRenderDevice device;
    auto baseOverlay = std::make_unique<RasterOverlay>(
        std::make_unique<DebugImageryProvider>(),
        TileScheme::createGeographicTMS(),
        makeRasterOverlayOptions());
    ActivatedRasterOverlay baseActivated(*baseOverlay);
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(tileset, rootKey);
    ASSERT_NE(nullptr, root);

    TilesetTestAccess::putTerrainCache(
        tileset,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(tileset, *root);
    ASSERT_TRUE(root->hasSurfaceDrawable());

    TilesetTestAccess::beginTilePlan(tileset);
    TilesetTestAccess::addTileToCurrentPlan(tileset, *root);
    ASSERT_EQ(1u, tileset.tilePlan().renderEntries.size());
    EXPECT_EQ(rootKey, tileset.tilePlan().renderEntries.front().selectedKey);
    EXPECT_EQ(rootKey, tileset.tilePlan().renderEntries.front().renderKey);

    TilesetTestAccess::prefetchRasterOverlays(tileset, *root);
    RasterMappedToTilesetTile* rootMapped =
        root->rasterOverlayState.mappings().empty()
            ? nullptr
            : root->rasterOverlayState.mappings()[0].get();
    RasterOverlayTile* rootRaster =
        rootMapped ? rootMapped->getLoadingTile() : nullptr;
    ASSERT_NE(nullptr, rootRaster);
    rootRaster->setTexture(std::make_unique<DummyTexture>(4, 4));
    rootRaster->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    TilesetTestAccess::prefetchRasterOverlays(tileset, *root);

    TilesetTestAccess::beginTilePlan(tileset);
    TilesetTestAccess::addTileToCurrentPlan(tileset, *root);
    ASSERT_EQ(1u, tileset.tilePlan().renderEntries.size());
    EXPECT_EQ(rootKey, tileset.tilePlan().renderEntries.front().selectedKey);
    EXPECT_EQ(rootKey, tileset.tilePlan().renderEntries.front().renderKey);
}

TEST(SceneFrameStateTest, PresentationTraceCopiesRenderEntryPassFailures) {
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});
    TilePlan& plan = TilesetTestAccess::mutableTilePlan(tileset);
    plan.renderEntryCommandMissedDrawCount = 5;
    plan.renderEntrySelectedCommandMissedDrawCount = 2;
    plan.renderEntryFadingCommandMissedDrawCount = 3;
    plan.renderEntryCommandDeferredCount = 7;
    plan.renderEntrySelectedCommandDeferredCount = 4;
    plan.renderEntryFadingCommandDeferredCount = 3;

    FrameState frameState;
    frameState.frameId = 3;
    RenderCommandList commands;
    const PresentationTrace trace = ScenePresentationTraceBuilder::build(
        ScenePresentationTraceInput{
            frameState,
            &tileset,
            emptyContentTilesets(),
            commands});

    ASSERT_EQ(1u, trace.tilesets.size());
    const PresentationTilesetTrace& tilesetTrace = trace.tilesets.front();
    EXPECT_EQ(5, tilesetTrace.renderEntryCommandMissedDrawCount);
    EXPECT_EQ(2, tilesetTrace.renderEntrySelectedCommandMissedDrawCount);
    EXPECT_EQ(3, tilesetTrace.renderEntryFadingCommandMissedDrawCount);
    EXPECT_EQ(7, tilesetTrace.renderEntryCommandDeferredCount);
    EXPECT_EQ(4, tilesetTrace.renderEntrySelectedCommandDeferredCount);
    EXPECT_EQ(3, tilesetTrace.renderEntryFadingCommandDeferredCount);
}

TEST(SceneFrameStateTest, PresentationTraceExposesFadingRenderEntry) {
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});
    TilePlan& plan = TilesetTestAccess::mutableTilePlan(tileset);
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TileRenderEntry entry;
    entry.selectedKey = rootKey;
    entry.renderKey = rootKey;
    entry.reason = TileRenderEntryReason::FadingOut;
    entry.opacity = 0.75f;
    entry.selectedThisFrame = false;
    plan.renderEntries.push_back(entry);
    plan.renderEntryPlannedCommandCount = 1;
    plan.renderEntrySelectedPlannedCommandCount = 0;
    plan.renderEntryFadingPlannedCommandCount = 1;
    plan.renderEntryCommandDrawCount = 1;
    plan.renderEntrySelectedCommandDrawCount = 0;
    plan.renderEntryFadingCommandDrawCount = 1;

    FrameState frameState;
    frameState.frameId = 7;
    RenderCommandList commands;
    const PresentationTrace trace = ScenePresentationTraceBuilder::build(
        ScenePresentationTraceInput{
            frameState,
            &tileset,
            emptyContentTilesets(),
            commands});

    ASSERT_EQ(1u, trace.tilesets.size());
    const PresentationTilesetTrace& tilesetTrace = trace.tilesets.front();
    ASSERT_EQ(1u, tilesetTrace.renderEntries.size());
    const PresentationRenderEntryTrace& entryTrace =
        tilesetTrace.renderEntries.front();
    EXPECT_FALSE(entryTrace.selectedThisFrame);
    EXPECT_EQ(TileRenderEntryReason::FadingOut, entryTrace.reason);
    EXPECT_NEAR(0.75f, entryTrace.opacity, 1e-6f);
    EXPECT_EQ(1, tilesetTrace.renderEntryPlannedCommandCount);
    EXPECT_EQ(0, tilesetTrace.renderEntrySelectedPlannedCommandCount);
    EXPECT_EQ(1, tilesetTrace.renderEntryFadingPlannedCommandCount);
    EXPECT_EQ(1, tilesetTrace.renderEntryCommandDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntrySelectedCommandDrawCount);
    EXPECT_EQ(1, tilesetTrace.renderEntryFadingCommandDrawCount);
}

TEST(SceneFrameStateTest, PresentationTraceExposesAdditiveSelectedEntries) {
    Tileset tileset(
        TileScheme::createGeographicTMS(),
        {},
        nullptr,
        TilesetOptions{});
    TilePlan& plan = TilesetTestAccess::mutableTilePlan(tileset);
    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childKey{"Geographic-TMS", 1, 0, 0};
    TileRenderEntry rootEntry;
    rootEntry.selectedKey = rootKey;
    rootEntry.renderKey = rootKey;
    TileRenderEntry childEntry;
    childEntry.selectedKey = childKey;
    childEntry.renderKey = childKey;
    plan.renderEntries = {rootEntry, childEntry};
    plan.renderEntryPlannedCommandCount = 2;
    plan.renderEntrySelectedPlannedCommandCount = 2;
    plan.renderEntryFadingPlannedCommandCount = 0;
    plan.renderEntryCommandDrawCount = 2;
    plan.renderEntrySelectedCommandDrawCount = 2;
    plan.renderEntryFadingCommandDrawCount = 0;

    FrameState frameState;
    frameState.frameId = 11;
    RenderCommandList commands;
    const PresentationTrace trace = ScenePresentationTraceBuilder::build(
        ScenePresentationTraceInput{
            frameState,
            &tileset,
            emptyContentTilesets(),
            commands});

    ASSERT_EQ(1u, trace.tilesets.size());
    const PresentationTilesetTrace& tilesetTrace = trace.tilesets.front();
    ASSERT_EQ(2u, tilesetTrace.renderEntries.size());
    EXPECT_EQ(rootKey, tilesetTrace.renderEntries[0].selectedKey);
    EXPECT_EQ(childKey, tilesetTrace.renderEntries[1].selectedKey);
    EXPECT_TRUE(tilesetTrace.renderEntries[0].selectedThisFrame);
    EXPECT_TRUE(tilesetTrace.renderEntries[1].selectedThisFrame);
    EXPECT_EQ(2, tilesetTrace.renderEntryPlannedCommandCount);
    EXPECT_EQ(2, tilesetTrace.renderEntrySelectedPlannedCommandCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryFadingPlannedCommandCount);
    EXPECT_EQ(2, tilesetTrace.renderEntryCommandDrawCount);
    EXPECT_EQ(2, tilesetTrace.renderEntrySelectedCommandDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryFadingCommandDrawCount);
    EXPECT_EQ(0, tilesetTrace.renderEntryAncestorFallbackCount);
}

TEST(SceneFrameStateTest, ClippedFallbackCommandsUseSelectedChildStableKeys) {
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
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    const TileKey childAKey{"Geographic-TMS", 1, 0, 0};
    const TileKey childBKey{"Geographic-TMS", 1, 1, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    TilesetTile* childA =
        TilesetTestAccess::ensureTile(*terrainRaw, childAKey);
    TilesetTile* childB =
        TilesetTestAccess::ensureTile(*terrainRaw, childBKey);
    ASSERT_NE(nullptr, root);
    ASSERT_NE(nullptr, childA);
    ASSERT_NE(nullptr, childB);

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
    ASSERT_NE(nullptr, rootRaster);
    rootRaster->setTexture(std::make_unique<DummyTexture>(4, 4));
    rootRaster->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);

    scene.setTileset(std::move(terrainTileset));
    scene.update(1.0 / 60.0);
    TilesetTestAccess::setInteractionActiveForFrame(*terrainRaw, true);
    TilesetTestAccess::beginTilePlan(*terrainRaw);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *childA);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *childB);
    scene.render();

    std::vector<std::string> clippedStableKeys;
    for (const RenderCommand& command : device.submittedCommands) {
        if (command.kind != RenderCommandKind::SurfaceTile ||
            command.surfaceClipEnabled <= 0.5f) {
            continue;
        }
        clippedStableKeys.push_back(command.stableKey);
    }

    ASSERT_EQ(2u, clippedStableKeys.size());
    EXPECT_NE(clippedStableKeys[0], clippedStableKeys[1]);
    EXPECT_NE(
        std::string::npos,
        clippedStableKeys[0].find("clip:Geographic-TMS/1/"));
    EXPECT_NE(
        std::string::npos,
        clippedStableKeys[1].find("clip:Geographic-TMS/1/"));
}

TEST(SceneFrameStateTest, SurfaceCommandUsesNoSkirtTerrainIndexRange) {
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
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    ASSERT_NE(nullptr, root);

    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(*terrainRaw, *root);
    SurfaceTileMesh* mesh = root->content.renderContent.mutableSurfaceMesh();
    ASSERT_NE(nullptr, mesh);
    ASSERT_GE(mesh->indices.size(), 12u);
    mesh->skirtMeta.noSkirtIndicesBegin = 0;
    mesh->skirtMeta.noSkirtIndicesCount =
        static_cast<uint32_t>(mesh->indices.size() - 6u);
    mesh->skirtMeta.noSkirtVerticesBegin = 0;
    mesh->skirtMeta.noSkirtVerticesCount =
        static_cast<uint32_t>(mesh->vertices.size());

    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    RasterMappedToTilesetTile* rootMapped =
        root->rasterOverlayState.mappings().empty()
            ? nullptr
            : root->rasterOverlayState.mappings()[0].get();
    RasterOverlayTile* rootRaster =
        rootMapped ? rootMapped->getLoadingTile() : nullptr;
    ASSERT_NE(nullptr, rootRaster);
    rootRaster->setTexture(std::make_unique<DummyTexture>(4, 4));
    rootRaster->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);

    scene.setTileset(std::move(terrainTileset));
    scene.update(1.0 / 60.0);
    TilesetTestAccess::setInteractionActiveForFrame(*terrainRaw, true);
    TilesetTestAccess::beginTilePlan(*terrainRaw);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *root);
    scene.render();

    const auto surfaceCommandIt = std::find_if(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& command) {
            return command.kind == RenderCommandKind::SurfaceTile;
        });
    ASSERT_NE(device.submittedCommands.end(), surfaceCommandIt);
    const RenderCommand& command = *surfaceCommandIt;
    EXPECT_EQ(0, command.indexOffset);
    EXPECT_EQ(
        static_cast<int>(mesh->skirtMeta.noSkirtIndicesCount),
        command.indexCount);
    EXPECT_EQ(
        static_cast<int>(mesh->indices.size()),
        command.surfaceMeshIndexCount);
    EXPECT_EQ(command.indexCount, command.surfaceNoSkirtIndexCount);
    EXPECT_EQ(6, command.surfaceSkirtIndexCount);
}

TEST(SceneFrameStateTest, SurfaceCommandSkipsExplicitMeshMissingIndexBuffer) {
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

    auto terrainTileset = std::make_unique<Tileset>(
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    ASSERT_NE(nullptr, root);

    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(*terrainRaw, *root);
    SurfaceTileMesh* mesh = root->content.renderContent.mutableSurfaceMesh();
    ASSERT_NE(nullptr, mesh);
    ASSERT_FALSE(mesh->vertices.empty());
    ASSERT_FALSE(mesh->indices.empty());

    BufferDesc vertexBufferDesc;
    vertexBufferDesc.size = mesh->vertices.size() * sizeof(SurfaceVertex);
    vertexBufferDesc.data = mesh->vertices.data();
    vertexBufferDesc.type = BufferDesc::Type::Vertex;
    root->content.renderContent.setSurfaceGpuBuffers(
        device.createBuffer(vertexBufferDesc),
        nullptr);

    scene.setTileset(std::move(terrainTileset));
    scene.update(1.0 / 60.0);
    TilesetTestAccess::setInteractionActiveForFrame(*terrainRaw, true);
    TilesetTestAccess::beginTilePlan(*terrainRaw);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *root);
    scene.render();

    const bool submittedSurfaceTile = std::any_of(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& command) {
            return command.kind == RenderCommandKind::SurfaceTile;
        });
    EXPECT_FALSE(submittedSurfaceTile);
}

TEST(SceneFrameStateTest, SurfaceCommandIgnoresOverflowingNoSkirtRange) {
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
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    ASSERT_NE(nullptr, root);

    TilesetTestAccess::putTerrainCache(
        *terrainRaw,
        rootKey,
        makeFlatHeightmap(0.0f));
    TilesetTestAccess::ensureTileMesh(*terrainRaw, *root);
    SurfaceTileMesh* mesh = root->content.renderContent.mutableSurfaceMesh();
    ASSERT_NE(nullptr, mesh);
    ASSERT_FALSE(mesh->indices.empty());
    mesh->skirtMeta.noSkirtIndicesBegin =
        std::numeric_limits<uint32_t>::max();
    mesh->skirtMeta.noSkirtIndicesCount = 2;

    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);
    RasterMappedToTilesetTile* rootMapped =
        root->rasterOverlayState.mappings().empty()
            ? nullptr
            : root->rasterOverlayState.mappings()[0].get();
    RasterOverlayTile* rootRaster =
        rootMapped ? rootMapped->getLoadingTile() : nullptr;
    ASSERT_NE(nullptr, rootRaster);
    rootRaster->setTexture(std::make_unique<DummyTexture>(4, 4));
    rootRaster->setMoreDetailAvailable(
        RasterOverlayTile::MoreDetailAvailable::No);
    TilesetTestAccess::prefetchRasterOverlays(*terrainRaw, *root);

    scene.setTileset(std::move(terrainTileset));
    scene.update(1.0 / 60.0);
    TilesetTestAccess::setInteractionActiveForFrame(*terrainRaw, true);
    TilesetTestAccess::beginTilePlan(*terrainRaw);
    TilesetTestAccess::addTileToCurrentPlan(*terrainRaw, *root);
    scene.render();

    const auto surfaceCommandIt = std::find_if(
        device.submittedCommands.begin(),
        device.submittedCommands.end(),
        [](const RenderCommand& command) {
            return command.kind == RenderCommandKind::SurfaceTile;
        });
    ASSERT_NE(device.submittedCommands.end(), surfaceCommandIt);
    const RenderCommand& command = *surfaceCommandIt;
    EXPECT_EQ(0, command.indexOffset);
    EXPECT_EQ(static_cast<int>(mesh->indices.size()), command.indexCount);
    EXPECT_EQ(
        static_cast<int>(mesh->indices.size()),
        command.surfaceNoSkirtIndexCount);
    EXPECT_EQ(0, command.surfaceSkirtIndexCount);
}

TEST(SceneFrameStateTest, GltfTerrainDiagnosticsDoNotUseLegacySurfacePrep) {
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
        TileScheme::createGeographicTMS(),
        std::vector<ActivatedRasterOverlay*>{&baseActivated},
        &device,
        TilesetOptions{});
    Tileset* terrainRaw = terrainTileset.get();

    const TileKey rootKey{"Geographic-TMS", 0, 0, 0};
    TilesetTile* root = TilesetTestAccess::ensureTile(*terrainRaw, rootKey);
    ASSERT_NE(root, nullptr);

    TilesetTestAccess::setLoadedGltfTerrainContent(
        *root,
        makeFlatGeographicTerrainGltfModel(root->bounds, 0.0));
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
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSynchronousPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDeferredPrep, 0);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesSelectedDrawn, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderEntriesMissed, 0);
    EXPECT_EQ(scene.diagnostics().terrainSurfaceCommandsSubmitted, 0);
    EXPECT_EQ(scene.diagnostics().renderGltfPrimitives, 1);
    EXPECT_EQ(scene.diagnostics().terrainRenderContentCommands, 1);
    EXPECT_EQ(scene.diagnostics().globeFallbackCommands, 1);
    EXPECT_EQ(scene.diagnostics().globeFallbackMaskedTerrainEntries, 1);
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
