#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/scene/FrameState.h"
#include "earth_engine/scene/Frustum.h"
#include "earth_engine/scene/SceneFrameDiagnostics.h"
#include "earth_engine/scene/Scene.h"

#include <vector>

using namespace earth_engine;

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
