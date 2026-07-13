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
    const int peakRasterPendingUploadBytes = std::max(
        diagnostics_.peakRasterPendingUploadBytes,
        diagnostics.peakRasterPendingUploadBytes);
    const int peakRasterCachedSourceTileBytes = std::max(
        diagnostics_.peakRasterCachedSourceTileBytes,
        diagnostics.peakRasterCachedSourceTileBytes);
    diagnostics_ = diagnostics;
    diagnostics_.peakRasterPendingUploadBytes =
        peakRasterPendingUploadBytes;
    diagnostics_.peakRasterCachedSourceTileBytes =
        peakRasterCachedSourceTileBytes;
}

void SceneTelemetryCoordinator::updatePresentationTrace(
    const FrameState& frameState,
    const Tileset* terrainTileset,
    const std::vector<std::unique_ptr<Tileset>>& additionalTilesets,
    const RenderCommandList& renderCommands) {
    presentationTrace_ = ScenePresentationTraceBuilder::build(
        ScenePresentationTraceInput{
            frameState,
            terrainTileset,
            additionalTilesets,
            renderCommands});
}

} // namespace earth_engine
