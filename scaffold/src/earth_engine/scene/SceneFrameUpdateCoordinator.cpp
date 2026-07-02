#include "SceneFrameUpdateCoordinator.h"

#include "Diagnostics.h"
#include "SceneFrameDiagnostics.h"
#include "SceneFrameStateBuilder.h"
#include "SceneTilesetCoordinator.h"
#include "../camera/CameraController.h"
#include "../debug/PerfTimer.h"
#include "../debug/PlatformLog.h"

#include <cstdio>

namespace earth_engine {

void SceneFrameUpdateCoordinator::update(
    const SceneFrameUpdateInput& input) {
    const double updateStartMs = perf::nowMs();

    const double t0 = perf::nowMs();
    SceneFrameDiagnostics::resetPerFrame(input.diagnostics);
    SceneFrameDiagnostics::updateFrameRate(
        input.diagnostics,
        input.deltaSeconds);
    const double t_reset = perf::nowMs();

    double cameraUpdateMs = 0.0;
    if (input.cameraController) {
        const double startMs = perf::nowMs();
        input.cameraController->update(input.deltaSeconds);
        cameraUpdateMs = perf::nowMs() - startMs;
    }
    const double t_cam = perf::nowMs();

    input.elapsedTime += input.deltaSeconds;
    SceneFrameStateBuildResult frameStateResult =
        SceneFrameStateBuilder::build(SceneFrameStateBuildInput{
            input.frameState,
            input.camera,
            ++input.frameId,
            input.elapsedTime,
            input.deltaSeconds,
            input.hasSelectorViewOverride,
            input.selectorViewOverride,
            input.hasInteractionFocus,
            input.interactionFocusDirection,
            input.interactionFocusTimeSeconds,
            input.timeController,
            input.skyGradient});
    const double t_fsb = perf::nowMs();
    input.diagnostics.cameraUpdateMs = cameraUpdateMs;
    input.diagnostics.environmentUpdateMs =
        frameStateResult.environmentUpdateMs;

    SceneTilesetUpdateResult tilesetUpdateResult =
        input.tilesets.update(input.frameState, input.pPrepRenderer);
    const double t_tsu = perf::nowMs();
    input.diagnostics.terrainUpdateMs = tilesetUpdateResult.terrainUpdateMs;
    input.diagnostics.contentTilesetUpdateMs =
        tilesetUpdateResult.contentTilesetUpdateMs;

    const double totalMs = perf::nowMs() - updateStartMs;
    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "camera=%.2f env=%.2f basemap=%.2f terrain=%.2f content=%.2f",
        input.diagnostics.cameraUpdateMs,
        input.diagnostics.environmentUpdateMs,
        input.diagnostics.basemapStackUpdateMs,
        input.diagnostics.terrainUpdateMs,
        input.diagnostics.contentTilesetUpdateMs);
    perf::logTiming(input.frameState.frameId,
                    "Scene.update.total",
                    totalMs,
                    detail);
    // Log per-section timing every frame if total > 30ms
    if (totalMs > 30.0) {
        platformLog(LogLevel::Info, "EarthPerf",
            "Scene.breakdown: reset=%.2f cam=%.2f fsb=%.2f tsu=%.2f total=%.2f",
            t_reset - t0, t_cam - t_reset, t_fsb - t_cam, t_tsu - t_fsb, totalMs);
    }
}

} // namespace earth_engine
