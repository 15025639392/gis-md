#include "SceneRenderDiagnostics.h"

#include "../renderer/RenderDevice.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

namespace earth_engine {

namespace {

void applyRenderCommandSnapshot(
    const SceneRenderCommandDiagnosticsSnapshot& snapshot,
    Diagnostics& diagnostics) {
    diagnostics.drawCalls = snapshot.drawCalls;
    diagnostics.gpuTextureCount = snapshot.gpuTextureCount;
    diagnostics.renderSurfaceTiles = snapshot.renderSurfaceTiles;
    diagnostics.renderGltfPrimitives = snapshot.renderGltfPrimitives;
    diagnostics.terrainRenderContentCommands =
        snapshot.terrainRenderContentCommands;
    diagnostics.surfaceMeshCount = snapshot.surfaceMeshCount;
    diagnostics.imageryExactAttachments = snapshot.imageryExactAttachments;
    diagnostics.imageryMissingTiles = snapshot.imageryMissingTiles;
    diagnostics.imageryMinTargetZoom = snapshot.imageryMinTargetZoom;
    diagnostics.imageryMaxTargetZoom = snapshot.imageryMaxTargetZoom;
    diagnostics.imageryMinTextureZoom = snapshot.imageryMinTextureZoom;
    diagnostics.imageryMaxTextureZoom = snapshot.imageryMaxTextureZoom;
    diagnostics.terrainSurfaceTileCommands =
        snapshot.terrainSurfaceTileCommands;
    diagnostics.terrainGltfPrimitiveCommands =
        snapshot.terrainGltfPrimitiveCommands;
}

void applySurfaceGenerationSnapshot(
    const SceneSurfaceCommandGenerationDiagnosticsSnapshot& snapshot,
    Diagnostics& diagnostics) {
    diagnostics.staleSurfaceCommands = snapshot.staleSurfaceCommands;
    diagnostics.missingGenerationSurfaceCommands =
        snapshot.missingGenerationSurfaceCommands;
    diagnostics.minSurfaceGeneration = snapshot.minSurfaceGeneration;
    diagnostics.maxSurfaceGeneration = snapshot.maxSurfaceGeneration;
}

} // namespace

SceneRenderCommandDiagnosticsSnapshot
SceneRenderCommandDiagnosticsSnapshot::fromCommands(
    const RenderCommandList& commands) {
    SceneRenderCommandDiagnosticsSnapshot snapshot;
    snapshot.drawCalls = static_cast<int>(commands.size());

    std::unordered_set<const Texture*> surfaceTextures;
    bool sawSurfaceGeometryZoom = false;
    bool sawSurfaceTextureZoom = false;

    for (const RenderCommand& command : commands) {
        if (command.kind == RenderCommandKind::GltfPrimitive) {
            if (!command.textures.empty()) {
                ++snapshot.imageryExactAttachments;
                for (const Texture* texture : command.textures) {
                    if (texture) {
                        surfaceTextures.insert(texture);
                    }
                }
                if (command.surfaceGeometryZoom >= 0) {
                    if (!sawSurfaceGeometryZoom) {
                        snapshot.imageryMinTargetZoom =
                            command.surfaceGeometryZoom;
                        snapshot.imageryMaxTargetZoom =
                            command.surfaceGeometryZoom;
                        sawSurfaceGeometryZoom = true;
                    } else {
                        snapshot.imageryMinTargetZoom =
                            std::min(snapshot.imageryMinTargetZoom,
                                     command.surfaceGeometryZoom);
                        snapshot.imageryMaxTargetZoom =
                            std::max(snapshot.imageryMaxTargetZoom,
                                     command.surfaceGeometryZoom);
                    }
                }
                if (command.surfaceTextureZoom >= 0) {
                    if (!sawSurfaceTextureZoom) {
                        snapshot.imageryMinTextureZoom =
                            command.surfaceTextureZoom;
                        snapshot.imageryMaxTextureZoom =
                            command.surfaceTextureZoom;
                        sawSurfaceTextureZoom = true;
                    } else {
                        snapshot.imageryMinTextureZoom =
                            std::min(snapshot.imageryMinTextureZoom,
                                     command.surfaceTextureZoom);
                        snapshot.imageryMaxTextureZoom =
                            std::max(snapshot.imageryMaxTextureZoom,
                                     command.surfaceTextureZoom);
                    }
                }
            } else {
                ++snapshot.imageryMissingTiles;
            }
            ++snapshot.renderGltfPrimitives;
            if (command.terrainRenderContent) {
                ++snapshot.terrainRenderContentCommands;
                ++snapshot.terrainGltfPrimitiveCommands;
                ++snapshot.terrainSurfaceTileCommands;
                ++snapshot.renderSurfaceTiles;
                ++snapshot.surfaceMeshCount;
            }
        } else if (command.kind == RenderCommandKind::GltfPrimitiveInstanced) {
            ++snapshot.renderGltfPrimitives;
            if (command.terrainRenderContent) {
                ++snapshot.terrainRenderContentCommands;
                ++snapshot.terrainGltfPrimitiveCommands;
                ++snapshot.terrainSurfaceTileCommands;
                ++snapshot.renderSurfaceTiles;
                ++snapshot.surfaceMeshCount;
            }
        }
    }

    snapshot.gpuTextureCount = static_cast<int>(surfaceTextures.size());
    return snapshot;
}

SceneSurfaceCommandGenerationDiagnosticsSnapshot
SceneSurfaceCommandGenerationDiagnosticsSnapshot::fromCommands(
    const RenderCommandList& commands,
    uint64_t expectedFrameId) {
    SceneSurfaceCommandGenerationDiagnosticsSnapshot snapshot;
    uint64_t minGeneration = std::numeric_limits<uint64_t>::max();
    uint64_t maxGeneration = 0;
    bool sawGeneration = false;

    for (const RenderCommand& command : commands) {
        if (command.kind != RenderCommandKind::GltfPrimitive) {
            continue;
        }
        if (expectedFrameId != 0 && command.frameId != expectedFrameId) {
            ++snapshot.staleSurfaceCommands;
        }
        if (command.generation == 0) {
            ++snapshot.missingGenerationSurfaceCommands;
            continue;
        }

        minGeneration = std::min(minGeneration, command.generation);
        maxGeneration = std::max(maxGeneration, command.generation);
        sawGeneration = true;
    }

    if (sawGeneration) {
        snapshot.minSurfaceGeneration = minGeneration;
        snapshot.maxSurfaceGeneration = maxGeneration;
    }
    return snapshot;
}

void SceneRenderDiagnostics::resetRenderCommandFields(
    Diagnostics& diagnostics) {
    diagnostics.gpuTextureCount = 0;
    diagnostics.renderSurfaceTiles = 0;
    diagnostics.renderGltfPrimitives = 0;
    diagnostics.terrainRenderContentCommands = 0;
    diagnostics.terrainRenderEntriesPlanned = 0;
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
    diagnostics.terrainSurfaceCommandsSubmitted = 0;
    diagnostics.globeFallbackCommands = 0;
    diagnostics.globeFallbackMaskedTerrainEntries = 0;
    diagnostics.surfaceMeshCount = 0;
    diagnostics.imageryAttachments = 0;
    diagnostics.imageryExactAttachments = 0;
    diagnostics.imageryParentFallbackAttachments = 0;
    diagnostics.imageryMissingTiles = 0;
    diagnostics.imageryUnsupportedTiles = 0;
    diagnostics.imageryTransitionTiles = 0;
    diagnostics.imageryKickedTiles = 0;
    diagnostics.imageryAncestorRetainedTiles = 0;
    diagnostics.imageryMinTargetZoom = 0;
    diagnostics.imageryMaxTargetZoom = 0;
    diagnostics.imageryMinTextureZoom = 0;
    diagnostics.imageryMaxTextureZoom = 0;
    diagnostics.terrainGeneration = 0;
    diagnostics.terrainSurfaceTileCommands = 0;
    diagnostics.terrainGltfPrimitiveCommands = 0;
    diagnostics.terrainParentFallbackMeshes = 0;
    diagnostics.terrainTransitionSurfaceMeshes = 0;
    diagnostics.ellipsoidSurfaceMeshes = 0;
}

void SceneRenderDiagnostics::updateSurfaceCommandGeneration(
    const RenderCommandList& commands,
    uint64_t expectedFrameId,
    Diagnostics& diagnostics) {
    diagnostics.staleSurfaceCommands = 0;
    diagnostics.missingGenerationSurfaceCommands = 0;
    diagnostics.minSurfaceGeneration = 0;
    diagnostics.maxSurfaceGeneration = 0;
    applySurfaceGenerationSnapshot(
        SceneSurfaceCommandGenerationDiagnosticsSnapshot::fromCommands(
            commands,
            expectedFrameId),
        diagnostics);
}

void SceneRenderDiagnostics::addRenderCommands(
    const RenderCommandList& commands,
    Diagnostics& diagnostics) {
    resetRenderCommandFields(diagnostics);
    applyRenderCommandSnapshot(
        SceneRenderCommandDiagnosticsSnapshot::fromCommands(commands),
        diagnostics);
}

void SceneRenderDiagnostics::finalizeRenderCommandFields(
    Diagnostics& diagnostics) {
    if (diagnostics.gpuTextureCount == 0) {
        diagnostics.gpuTextureCount = diagnostics.cachedTextures;
    }
    diagnostics.imageryAttachments =
        diagnostics.imageryExactAttachments +
        diagnostics.imageryParentFallbackAttachments;
}

} // namespace earth_engine
