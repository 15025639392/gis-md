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
    diagnostics.imageryAncestor1Attachments =
        snapshot.imageryAncestor1Attachments;
    diagnostics.imageryAncestor2Attachments =
        snapshot.imageryAncestor2Attachments;
    diagnostics.imageryAncestor3PlusAttachments =
        snapshot.imageryAncestor3PlusAttachments;
    // 旧的二值 parent 字段无人赋值恒 0,现聚合成「用了祖先上采样的总片数」,
    // 分档细节看上面三个。
    diagnostics.imageryParentFallbackAttachments =
        snapshot.imageryAncestor1Attachments +
        snapshot.imageryAncestor2Attachments +
        snapshot.imageryAncestor3PlusAttachments;
    diagnostics.imageryMissingTiles = snapshot.imageryMissingTiles;
    diagnostics.imageryMinTargetZoom = snapshot.imageryMinTargetZoom;
    diagnostics.imageryMaxTargetZoom = snapshot.imageryMaxTargetZoom;
    diagnostics.imageryMinTextureZoom = snapshot.imageryMinTextureZoom;
    diagnostics.imageryMaxTextureZoom = snapshot.imageryMaxTextureZoom;
    diagnostics.terrainSurfaceTileCommands =
        snapshot.terrainSurfaceTileCommands;
    diagnostics.terrainGltfPrimitiveCommands =
        snapshot.terrainGltfPrimitiveCommands;
    diagnostics.terrainSurfaceRealCommands =
        snapshot.terrainSurfaceRealCommands;
    diagnostics.terrainSurfaceFillProxyCommands =
        snapshot.terrainSurfaceFillProxyCommands;
    diagnostics.terrainSurfaceEllipsoidCommands =
        snapshot.terrainSurfaceEllipsoidCommands;
    diagnostics.terrainSurfaceUnknownCommands =
        snapshot.terrainSurfaceUnknownCommands;
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
        // 逐 draw 与地形合批两种命令走同一套统计。合批落地时曾把实例化分支
        // 拆成独立的一段,只抄了地形来源计数、漏了影像统计 —— 掠视 128 片
        // 可见瓦片因此只数到 35 片有影像(18 条批命令整段被跳过)。合并回一处
        // 避免再次漏抄。
        if (command.kind == RenderCommandKind::GltfPrimitive ||
            command.kind == RenderCommandKind::GltfPrimitiveInstanced) {
            if (!command.textures.empty()) {
                // 「有纹理」不等于「贴的是本级影像」——按 builder 记下的
                // 祖先层级差分档,这才是屏幕上糊的程度。delta<0 表示这条命令
                // 没有底图影像(例如只有材质纹理的非地形 glTF),不入直方图。
                //
                // 按实例数加权:地形合批后一条命令代表 instanceCount 片瓦片,
                // 不加权直方图会缩水成命令数(掠视 128 片只数到 35)。合批的
                // 资格闸要求成员同 {z,row} 模板且页存储全 cell 驻留、共享同一
                // 份纹理状态,故成员影像来源同档,用首实例的分类代表整批。
                // 注意只有本直方图按瓦片加权;drawCalls/terrainSurface* 等是
                // 绘制开销口径,仍按命令数计。
                const int delta = command.imageryAncestorLevelDelta;
                const int tiles =
                    command.kind == RenderCommandKind::GltfPrimitiveInstanced
                        ? std::max(1, command.instanceCount)
                        : 1;
                if (delta == 0) {
                    snapshot.imageryExactAttachments += tiles;
                } else if (delta == 1) {
                    snapshot.imageryAncestor1Attachments += tiles;
                } else if (delta == 2) {
                    snapshot.imageryAncestor2Attachments += tiles;
                } else if (delta >= 3) {
                    snapshot.imageryAncestor3PlusAttachments += tiles;
                }
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
            } else if (command.terrainRenderContent) {
                // 只有「地形瓦片没挂上影像」才算真空洞。极帽(PolarCapRenderer)
                // 复用 glTF primitive 且本就无纹理,此前被误计成缺影像瓦片 ——
                // 这就是面板上那个恒定不消失的「2 missing」的由来。
                ++snapshot.imageryMissingTiles;
            }
            ++snapshot.renderGltfPrimitives;
            if (command.terrainRenderContent) {
                ++snapshot.terrainRenderContentCommands;
                ++snapshot.terrainGltfPrimitiveCommands;
                ++snapshot.terrainSurfaceTileCommands;
                ++snapshot.renderSurfaceTiles;
                ++snapshot.surfaceMeshCount;
                switch (command.terrainSurfaceSource) {
                    case TerrainSurfaceCommandSource::RealTerrain:
                        ++snapshot.terrainSurfaceRealCommands;
                        break;
                    case TerrainSurfaceCommandSource::FillProxy:
                        ++snapshot.terrainSurfaceFillProxyCommands;
                        break;
                    case TerrainSurfaceCommandSource::EllipsoidFallback:
                        ++snapshot.terrainSurfaceEllipsoidCommands;
                        break;
                    case TerrainSurfaceCommandSource::Unknown:
                    default:
                        ++snapshot.terrainSurfaceUnknownCommands;
                        break;
                }
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
        if (command.kind != RenderCommandKind::GltfPrimitive ||
            !command.terrainRenderContent) {
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
    diagnostics.terrainSelectedForRenderTiles = 0;
    diagnostics.terrainRenderEntryDropClipUv = 0;
    diagnostics.terrainRenderEntryDropNotBuildable = 0;
    diagnostics.terrainRenderEntryDropNoGeometry = 0;
    diagnostics.terrainRenderEntryDropNoMapping = 0;
    diagnostics.terrainRenderEntryDropNoReadyTexture = 0;
    diagnostics.terrainRenderEntryDropTexcoordInvalid = 0;
    diagnostics.terrainRenderEntryDropOther = 0;
    diagnostics.terrainRenderEntryDropMinZoom = 0;
    diagnostics.terrainRenderEntryDropMaxZoom = 0;
    diagnostics.terrainRenderEntriesMissingSelected = 0;
    diagnostics.terrainRenderEntriesMissingRender = 0;
    diagnostics.terrainZeroDrawNoContentNoFill = 0;
    diagnostics.terrainZeroDrawFillNoCommands = 0;
    diagnostics.terrainZeroDrawContentNoCommands = 0;
    diagnostics.terrainSurfaceCommandsSubmitted = 0;
    diagnostics.terrainSurfaceRealCommands = 0;
    diagnostics.terrainSurfaceFillProxyCommands = 0;
    diagnostics.terrainSurfaceEllipsoidCommands = 0;
    diagnostics.terrainSurfaceUnknownCommands = 0;
    diagnostics.surfaceMeshCount = 0;
    diagnostics.imageryAttachments = 0;
    diagnostics.imageryExactAttachments = 0;
    diagnostics.imageryAncestor1Attachments = 0;
    diagnostics.imageryAncestor2Attachments = 0;
    diagnostics.imageryAncestor3PlusAttachments = 0;
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
