#pragma once

#include "Diagnostics.h"
#include "../renderer/RenderCommand.h"

#include <cstdint>

namespace earth_engine {

struct SceneRenderCommandDiagnosticsSnapshot {
    int drawCalls = 0;
    int gpuTextureCount = 0;
    int renderSurfaceTiles = 0;
    int renderGltfPrimitives = 0;
    int terrainRenderContentCommands = 0;
    int surfaceMeshCount = 0;
    int imageryExactAttachments = 0;
    int imageryMissingTiles = 0;
    int imageryMinTargetZoom = 0;
    int imageryMaxTargetZoom = 0;
    int imageryMinTextureZoom = 0;
    int imageryMaxTextureZoom = 0;
    int terrainSurfaceMeshes = 0;
    int terrainReadySurfaceMeshes = 0;

    static SceneRenderCommandDiagnosticsSnapshot fromCommands(
        const RenderCommandList& commands);
};

struct SceneSurfaceCommandGenerationDiagnosticsSnapshot {
    int staleSurfaceCommands = 0;
    int missingGenerationSurfaceCommands = 0;
    uint64_t minSurfaceGeneration = 0;
    uint64_t maxSurfaceGeneration = 0;

    static SceneSurfaceCommandGenerationDiagnosticsSnapshot fromCommands(
        const RenderCommandList& commands,
        uint64_t expectedFrameId);
};

class SceneRenderDiagnostics {
public:
    static void resetRenderCommandFields(Diagnostics& diagnostics);
    static void updateSurfaceCommandGeneration(const RenderCommandList& commands,
                                               uint64_t expectedFrameId,
                                               Diagnostics& diagnostics);
    static void addRenderCommands(const RenderCommandList& commands,
                                  Diagnostics& diagnostics);
    static void finalizeRenderCommandFields(Diagnostics& diagnostics);
};

} // namespace earth_engine
