#include "SceneTelemetryCoordinator.h"
#include "SceneFrameDiagnostics.h"
#include "ScenePresentationTraceBuilder.h"

namespace earth_engine {

void SceneTelemetryCoordinator::recordEngineTiming(
    EngineTimingScope scope,
    double elapsedMs) {
    SceneFrameDiagnostics::recordEngineTiming(
        diagnostics_,
        scope,
        elapsedMs);
}

void SceneTelemetryCoordinator::finishEngineFrame(double elapsedMs) {
    SceneFrameDiagnostics::finishEngineFrame(diagnostics_, elapsedMs);
}

void SceneTelemetryCoordinator::replaceRenderDiagnostics(
    const Diagnostics& diagnostics) {
    diagnostics_ = diagnostics;
}

void SceneTelemetryCoordinator::updatePresentationTrace(
    const ScenePresentationTraceInput& input) {
    presentationTrace_ = ScenePresentationTraceBuilder::build(input);
}

} // namespace earth_engine
