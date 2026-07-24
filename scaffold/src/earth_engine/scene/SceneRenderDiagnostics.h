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
    // 加载质量:底图影像的「糊几级」直方图。exact = 贴的是本级(清晰);
    // ancestor1/2/3plus = 目标层未到、退回祖先上采样,差 1/2/3+ 级;
    // missing = 地形瓦片压根没挂上底图影像(真空洞,极帽等非地形命令不计入)。
    int imageryExactAttachments = 0;
    int imageryAncestor1Attachments = 0;
    int imageryAncestor2Attachments = 0;
    int imageryAncestor3PlusAttachments = 0;
    int imageryMissingTiles = 0;
    int imageryMinTargetZoom = 0;
    int imageryMaxTargetZoom = 0;
    int imageryMinTextureZoom = 0;
    int imageryMaxTextureZoom = 0;
    int terrainSurfaceTileCommands = 0;
    int terrainGltfPrimitiveCommands = 0;
    int terrainSurfaceRealCommands = 0;
    int terrainSurfaceFillProxyCommands = 0;
    int terrainSurfaceEllipsoidCommands = 0;
    int terrainSurfaceUnknownCommands = 0;

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
