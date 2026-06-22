#include "SceneFrameUpdateCoordinator.h"

#include "Diagnostics.h"
#include "SceneFrameDiagnostics.h"
#include "SceneFrameStateBuilder.h"
#include "SceneTilesetCoordinator.h"
#include "../camera/CameraController.h"
#include "../debug/PerfTimer.h"

#include <cstdio>

namespace earth_engine {

void SceneFrameUpdateCoordinator::update(
    const SceneFrameUpdateInput& input) {
    const double updateStartMs = perf::nowMs();
    SceneFrameDiagnostics::resetPerFrame(input.diagnostics);
    SceneFrameDiagnostics::updateFrameRate(
        input.diagnostics,
        input.deltaSeconds);

    double cameraUpdateMs = 0.0;
    if (input.cameraController) {
        const double startMs = perf::nowMs();
        input.cameraController->update(input.deltaSeconds);
        cameraUpdateMs = perf::nowMs() - startMs;
    }

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
    input.diagnostics.cameraUpdateMs = cameraUpdateMs;
    input.diagnostics.environmentUpdateMs =
        frameStateResult.environmentUpdateMs;

    SceneTilesetUpdateResult tilesetUpdateResult =
        input.tilesets.update(input.frameState, input.pPrepRenderer);
    input.diagnostics.terrainUpdateMs = tilesetUpdateResult.terrainUpdateMs;
    input.diagnostics.contentTilesetUpdateMs =
        tilesetUpdateResult.contentTilesetUpdateMs;

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
                    perf::nowMs() - updateStartMs,
                    detail);
}

} // namespace earth_engine
