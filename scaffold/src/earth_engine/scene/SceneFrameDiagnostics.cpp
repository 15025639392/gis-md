#include "SceneFrameDiagnostics.h"

namespace earth_engine {
namespace {

constexpr double kFpsSmoothing = 0.1;

} // namespace

void SceneFrameDiagnostics::resetPerFrame(Diagnostics& diagnostics) {
    diagnostics.cameraUpdateMs = 0.0;
    diagnostics.environmentUpdateMs = 0.0;
    diagnostics.basemapStackUpdateMs = 0.0;
    diagnostics.terrainUpdateMs = 0.0;
    diagnostics.contentTilesetUpdateMs = 0.0;
    diagnostics.renderCommandBuildMs = 0.0;
    diagnostics.renderSubmitMs = 0.0;
}

void SceneFrameDiagnostics::updateFrameRate(
    Diagnostics& diagnostics,
    double deltaSeconds) {
    if (deltaSeconds <= 0.0) {
        return;
    }

    diagnostics.frameTimeMs = deltaSeconds * 1000.0;
    diagnostics.fps =
        diagnostics.fps * (1.0 - kFpsSmoothing) +
        (1.0 / deltaSeconds) * kFpsSmoothing;
}

void SceneFrameDiagnostics::recordEngineTiming(
    Diagnostics& diagnostics,
    EngineTimingScope scope,
    double elapsedMs) {
    switch (scope) {
        case EngineTimingScope::BeginFrame:
            diagnostics.engineBeginFrameMs = elapsedMs;
            break;
        case EngineTimingScope::SceneUpdate:
            diagnostics.sceneUpdateMs = elapsedMs;
            break;
        case EngineTimingScope::SceneRender:
            diagnostics.sceneRenderMs = elapsedMs;
            break;
        case EngineTimingScope::EndFrame:
            diagnostics.engineEndFrameMs = elapsedMs;
            break;
    }
}

void SceneFrameDiagnostics::finishEngineFrame(
    Diagnostics& diagnostics,
    double elapsedMs) {
    diagnostics.engineFrameCpuMs = elapsedMs;
}

} // namespace earth_engine
