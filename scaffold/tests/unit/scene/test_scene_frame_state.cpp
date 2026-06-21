#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/Frustum.h"
#include "earth_engine/scene/SceneFrameDiagnostics.h"
#include "earth_engine/scene/SceneFrameStateBuilder.h"
#include "earth_engine/scene/Scene.h"
#include "earth_engine/tiling/TileScheme.h"
#include "earth_engine/tiling/Tileset.h"

#include <vector>

using namespace earth_engine;

namespace earth_engine {
struct TilesetTestAccess {
    static TilesetTile* ensureTile(Tileset& tileset, const TileKey& key) {
        return tileset.contentAccess_.ensureTile(key);
    }

    static TileOcclusionState checkOcclusion(
        const Tileset& tileset,
        const TilesetTile& tile) {
        return tileset.checkOcclusion(tile);
    }
};
} // namespace earth_engine

namespace {

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
