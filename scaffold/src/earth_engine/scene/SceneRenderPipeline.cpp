#include "SceneRenderPipeline.h"
#include "Camera.h"
#include "SceneFrameDiagnosticsAggregator.h"
#include "SceneRenderCommandUniformUpdater.h"
#include "SceneRenderDiagnostics.h"
#include "../debug/PerfTimer.h"
#include "../environment/AtmosphereBackgroundPass.h"
#include "../environment/SkyBox.h"
#include "../environment/SkyGradient.h"
#include "../layers/VectorLayer.h"
#include "../renderer/Renderer.h"
#include "../tiling/Tileset.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace earth_engine {
namespace {

bool isTerrainSurfaceCommand(const RenderCommand& command) {
    return command.kind == RenderCommandKind::GltfPrimitive &&
           command.terrainRenderContent;
}

int countTerrainSurfaceCommands(const RenderCommandList& commands) {
    return static_cast<int>(
        std::count_if(
            commands.begin(),
            commands.end(),
            isTerrainSurfaceCommand));
}

int terrainRenderEntryCount(const Tileset* tileset) {
    if (!tileset) {
        return 0;
    }
    return static_cast<int>(tileset->tilePlan().renderEntries.size());
}

void applyTerrainRenderEntryDiagnostics(
    const Tileset* tileset,
    Diagnostics& diagnostics) {
    diagnostics.terrainRenderEntriesPlanned = terrainRenderEntryCount(tileset);
    diagnostics.terrainRenderEntriesSelectedPlanned = 0;
    diagnostics.terrainRenderEntriesFadingPlanned = 0;
    diagnostics.terrainRenderEntriesAncestorFallback = 0;
    diagnostics.terrainRenderEntriesSynchronousPrep = 0;
    diagnostics.terrainRenderEntriesDeferredPrep = 0;
    diagnostics.terrainRenderEntriesDrawn = 0;
    diagnostics.terrainRenderEntriesSelectedDrawn = 0;
    diagnostics.terrainRenderEntriesFadingDrawn = 0;
    diagnostics.terrainRenderEntriesMissed = 0;
    diagnostics.terrainRenderEntriesSelectedMissed = 0;
    diagnostics.terrainRenderEntriesFadingMissed = 0;
    diagnostics.terrainRenderEntriesDeferred = 0;
    diagnostics.terrainRenderEntriesSelectedDeferred = 0;
    diagnostics.terrainRenderEntriesFadingDeferred = 0;
    if (!tileset) {
        return;
    }

    const TilePlan& plan = tileset->tilePlan();
    diagnostics.terrainRenderEntriesSelectedPlanned =
        plan.renderEntrySelectedPlannedCommandCount;
    diagnostics.terrainRenderEntriesFadingPlanned =
        plan.renderEntryFadingPlannedCommandCount;
    diagnostics.terrainRenderEntriesAncestorFallback =
        plan.renderEntryAncestorFallbackCount;
    diagnostics.terrainRenderEntriesSynchronousPrep =
        plan.renderEntrySynchronousPrepCount;
    diagnostics.terrainRenderEntriesDeferredPrep =
        plan.renderEntryDeferredPrepCount;
    diagnostics.terrainRenderEntriesDrawn =
        plan.renderEntryCommandDrawCount;
    diagnostics.terrainRenderEntriesSelectedDrawn =
        plan.renderEntrySelectedCommandDrawCount;
    diagnostics.terrainRenderEntriesFadingDrawn =
        plan.renderEntryFadingCommandDrawCount;
    diagnostics.terrainRenderEntriesMissed =
        plan.renderEntryCommandMissedDrawCount;
    diagnostics.terrainRenderEntriesSelectedMissed =
        plan.renderEntrySelectedCommandMissedDrawCount;
    diagnostics.terrainRenderEntriesFadingMissed =
        plan.renderEntryFadingCommandMissedDrawCount;
    diagnostics.terrainRenderEntriesDeferred =
        plan.renderEntryCommandDeferredCount;
    diagnostics.terrainRenderEntriesSelectedDeferred =
        plan.renderEntrySelectedCommandDeferredCount;
    diagnostics.terrainRenderEntriesFadingDeferred =
        plan.renderEntryFadingCommandDeferredCount;
}

} // namespace

SceneRenderPipeline::Result SceneRenderPipeline::render(Context context) {
    const double renderStartMs = perf::nowMs();
    context.commands.clear();
    reserveCommands(context);

    double skyMs = 0.0;
    double atmosphereMs = 0.0;
    double layerCommandsMs = 0.0;
    double vectorCommandsMs = 0.0;
    double mvpUniformsMs = 0.0;
    double sortMs = 0.0;
    double surfaceDiagnosticsMs = 0.0;
    double validateMs = 0.0;
    double diagnosticsMs = 0.0;

    RenderCommandList stableLayerCommands =
        buildStableLayerCommands(context, layerCommandsMs);

    buildSkyCommands(context, skyMs);
    buildAtmosphereCommands(context, atmosphereMs);
    buildLayerCommands(
        context,
        stableLayerCommands,
        vectorCommandsMs);
    applyMvpUniforms(context, mvpUniformsMs);
    sortAndValidate(context, sortMs, surfaceDiagnosticsMs, validateMs);
    aggregateDiagnostics(context, diagnosticsMs);

    context.diagnostics.renderCommandBuildMs =
        perf::nowMs() - renderStartMs;

    if (context.beforeSubmit) {
        context.beforeSubmit();
    }

    const double submitStartMs = perf::nowMs();
    context.renderer.submit(context.commands);
    context.diagnostics.renderSubmitMs =
        perf::nowMs() - submitStartMs;

    releaseRenderReferences(context);

    char buildDetail[384];
    std::snprintf(buildDetail, sizeof(buildDetail),
        "sky=%.2f atmo=%.2f layers=%.2f vector=%.2f mvp=%.2f sort=%.2f surfDiag=%.2f validate=%.2f diag=%.2f commands=%zu",
        skyMs,
        atmosphereMs,
        layerCommandsMs,
        vectorCommandsMs,
        mvpUniformsMs,
        sortMs,
        surfaceDiagnosticsMs,
        validateMs,
        diagnosticsMs,
        context.commands.size());
    perf::logTiming(context.frameState.frameId,
                    "Scene.render.buildBreakdown",
                    context.diagnostics.renderCommandBuildMs,
                    buildDetail);

    char detail[192];
    std::snprintf(detail, sizeof(detail),
        "build=%.2f submit=%.2f draw=%d surface=%d mesh=%d",
        context.diagnostics.renderCommandBuildMs,
        context.diagnostics.renderSubmitMs,
        context.diagnostics.drawCalls,
        context.diagnostics.renderSurfaceTiles,
        context.diagnostics.surfaceMeshCount);
    perf::logTiming(context.frameState.frameId,
                    "Scene.render.total",
                    perf::nowMs() - renderStartMs,
                    detail);
    return Result{context.diagnostics};
}

void SceneRenderPipeline::reserveCommands(Context& context) const {
    size_t expectedCommands = 4 + context.vectorLayers.size() * 4;
    auto addExpectedTilesetCommands = [&](const Tileset* tileset) {
        if (!tileset) return;
        expectedCommands += tileset->tilePlan().renderEntries.size();
    };
    addExpectedTilesetCommands(context.terrainTileset);
    for (const auto& tileset : context.additionalTilesets) {
        addExpectedTilesetCommands(tileset.get());
    }
    if (context.commands.capacity() < expectedCommands) {
        context.commands.reserve(expectedCommands);
    }
}

void SceneRenderPipeline::buildSkyCommands(
    Context& context,
    double& skyMs) const {
    const double startMs = perf::nowMs();
    if (context.skyBox && context.skyBox->isReady() &&
        context.skyGradient && context.frameState.camera) {
        const auto& cam = *context.frameState.camera;
        Mat4 vm = cam.viewMatrix();
        const double* vmPtr = glm::value_ptr(vm.raw());
        float viewMatrix[16];
        for (int i = 0; i < 16; ++i) viewMatrix[i] = static_cast<float>(vmPtr[i]);

        const float vpW =
            static_cast<float>(context.frameState.viewportWidthPixels);
        const float vpH =
            static_cast<float>(context.frameState.viewportHeightPixels);
        Mat4 pm = cam.projectionMatrix(
            static_cast<double>(vpW), static_cast<double>(vpH));
        const double* pmPtr = glm::value_ptr(pm.raw());
        float projMatrix[16];
        for (int i = 0; i < 16; ++i) projMatrix[i] = static_cast<float>(pmPtr[i]);

        float nightFactor = static_cast<float>(
            context.skyGradient->sunElevation() < -0.05
                ? std::clamp(
                      std::exp(context.skyGradient->sunElevation() * 8.0),
                      0.0,
                      1.0)
                : 0.0);
        double spaceFactor =
            std::clamp((cam.getHeight() - 120000.0) / 780000.0, 0.0, 1.0);
        spaceFactor = spaceFactor * spaceFactor * (3.0 - 2.0 * spaceFactor);
        nightFactor = std::max(nightFactor, static_cast<float>(spaceFactor));

        context.commands.push_back(context.skyBox->buildCommand(
            viewMatrix, projMatrix, cam.isOrthographic(), nightFactor));
    }
    skyMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::buildAtmosphereCommands(
    Context& context,
    double& atmosphereMs) const {
    const double startMs = perf::nowMs();
    if (context.atmospherePass && context.atmospherePass->isReady() &&
        context.skyGradient && context.frameState.camera) {
        const auto& cam = *context.frameState.camera;
        const float vpW =
            static_cast<float>(context.frameState.viewportWidthPixels);
        const float vpH =
            static_cast<float>(context.frameState.viewportHeightPixels);
        Vec3 sunDir(context.frameState.lightDir.x,
                    context.frameState.lightDir.y,
                    context.frameState.lightDir.z);

        context.commands.push_back(context.atmospherePass->buildCommand(
            cam.position(),
            static_cast<float>(cam.verticalFovRadians()),
            static_cast<int>(vpW),
            static_cast<int>(vpH),
            cam.right(),
            cam.up(),
            cam.direction(),
            sunDir,
            context.skyGradient->parameters()));
    }
    atmosphereMs = perf::nowMs() - startMs;
}

RenderCommandList SceneRenderPipeline::buildStableLayerCommands(
    Context& context,
    double& layerCommandsMs) {
    const double layerStartMs = perf::nowMs();
    tileCommandCandidates_.clear();
    reserveCommands(context);
    auto appendTilesetCommands = [&](Tileset& tileset,
                                     const std::string& stablePrefix) {
        const size_t before = tileCommandCandidates_.size();
        tileset.buildRenderCommands(context.renderer, tileCommandCandidates_);
        for (size_t i = before; i < tileCommandCandidates_.size(); ++i) {
            RenderCommand& command = tileCommandCandidates_[i];
            if (!command.stableKey.empty()) {
                command.stableKey = stablePrefix + command.stableKey;
            }
        }
    };
    if (context.terrainTileset) {
        appendTilesetCommands(*context.terrainTileset, "terrain:");
    }
    size_t tilesetIndex = 0;
    for (auto& tileset : context.additionalTilesets) {
        if (tileset) {
            appendTilesetCommands(
                *tileset,
                "content:" + std::to_string(tilesetIndex) + ":");
        }
        ++tilesetIndex;
    }
    RenderCommandList activeTileCommands;
    activeTileCommands.reserve(tileCommandCandidates_.size());
    tileCommandSet_.update(
        tileCommandCandidates_,
        context.frameState.frameId,
        activeTileCommands);
    layerCommandsMs = perf::nowMs() - layerStartMs;
    return activeTileCommands;
}

void SceneRenderPipeline::buildLayerCommands(
    Context& context,
    const RenderCommandList& stableLayerCommands,
    double& vectorCommandsMs) const {
    context.commands.insert(
        context.commands.end(),
        stableLayerCommands.begin(),
        stableLayerCommands.end());

    const double vectorStartMs = perf::nowMs();
    for (auto& vLayer : context.vectorLayers) {
        if (vLayer->visible()) {
            vLayer->buildRenderCommands(
                context.frameState, context.renderer, context.commands);
        }
    }
    vectorCommandsMs = perf::nowMs() - vectorStartMs;
}

void SceneRenderPipeline::applyMvpUniforms(
    Context& context,
    double& mvpUniformsMs) const {
    const double startMs = perf::nowMs();
    SceneRenderCommandUniformUpdater::apply(context.frameState,
                                            context.commands);
    mvpUniformsMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::sortAndValidate(
    Context& context,
    double& sortMs,
    double& surfaceDiagnosticsMs,
    double& validateMs) const {
    const double sortStartMs = perf::nowMs();
    bool needsSort = false;
    bool hasTranslucentGltf = false;
    for (size_t i = 1; i < context.commands.size(); ++i) {
        if (context.commands[i - 1].blend &&
            (context.commands[i - 1].kind == RenderCommandKind::GltfPrimitive ||
             context.commands[i - 1].kind ==
                 RenderCommandKind::GltfPrimitiveInstanced)) {
            hasTranslucentGltf = true;
        }
        if (mvpRenderOrder(context.commands[i - 1].kind) >
            mvpRenderOrder(context.commands[i].kind)) {
            needsSort = true;
            break;
        }
    }
    if (!context.commands.empty()) {
        const RenderCommand& last = context.commands.back();
        if (last.blend &&
            (last.kind == RenderCommandKind::GltfPrimitive ||
             last.kind == RenderCommandKind::GltfPrimitiveInstanced)) {
            hasTranslucentGltf = true;
        }
    }
    if (needsSort || hasTranslucentGltf) {
        sortMvpRenderCommands(context.commands);
    }
    sortMs = perf::nowMs() - sortStartMs;

    const double surfaceStartMs = perf::nowMs();
    SceneRenderDiagnostics::updateSurfaceCommandGeneration(
        context.commands,
        context.frameState.frameId,
        context.diagnostics);
    surfaceDiagnosticsMs = perf::nowMs() - surfaceStartMs;

    const double validateStartMs = perf::nowMs();
    if (auto error = validateMvpRenderCommands(
            context.commands, context.frameState.frameId)) {
        throw std::runtime_error(
            "MVP render command validation failed for '" + error->owner +
            "': " + error->message);
    }
    validateMs = perf::nowMs() - validateStartMs;
}

void SceneRenderPipeline::aggregateDiagnostics(
    Context& context,
    double& diagnosticsMs) const {
    const double startMs = perf::nowMs();
    SceneFrameDiagnosticsAggregator::aggregateRenderFrame(
        context.commands,
        context.terrainTileset,
        context.additionalTilesets,
        context.diagnostics);
    applyTerrainRenderEntryDiagnostics(
        context.terrainTileset,
        context.diagnostics);
    context.diagnostics.terrainSurfaceCommandsSubmitted =
        countTerrainSurfaceCommands(context.commands);
    diagnosticsMs = perf::nowMs() - startMs;
}

void SceneRenderPipeline::releaseRenderReferences(Context& context) const {
    if (context.terrainTileset) {
        context.terrainTileset->releaseRenderReferences();
    }
    for (auto& tileset : context.additionalTilesets) {
        if (tileset) {
            tileset->releaseRenderReferences();
        }
    }
}

} // namespace earth_engine
