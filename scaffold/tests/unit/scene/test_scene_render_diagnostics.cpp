#include <gtest/gtest.h>

#include "earth_engine/scene/SceneRenderDiagnostics.h"

using namespace earth_engine;

TEST(SceneRenderCommandDiagnosticsSnapshotTest, CountsRenderCommandLanes) {
    Texture* sharedTexture = reinterpret_cast<Texture*>(0x1);
    Texture* secondTexture = reinterpret_cast<Texture*>(0x2);

    RenderCommand firstSurface;
    firstSurface.kind = RenderCommandKind::GltfPrimitive;
    firstSurface.terrainRenderContent = true;
    firstSurface.terrainSurfaceSource =
        TerrainSurfaceCommandSource::RealTerrain;
    firstSurface.textures = {sharedTexture};
    firstSurface.surfaceGeometryZoom = 3;
    firstSurface.surfaceTextureZoom = 4;
    firstSurface.imageryAncestorLevelDelta = 0;  // 贴的是本级影像 = 清晰

    RenderCommand secondSurface;
    secondSurface.kind = RenderCommandKind::GltfPrimitive;
    secondSurface.terrainRenderContent = true;
    secondSurface.terrainSurfaceSource =
        TerrainSurfaceCommandSource::FillProxy;
    secondSurface.textures = {sharedTexture, secondTexture};
    secondSurface.surfaceGeometryZoom = 7;
    secondSurface.surfaceTextureZoom = 6;
    secondSurface.imageryAncestorLevelDelta = 1;  // 退回父级上采样 = 糊 1 级

    RenderCommand missingImagerySurface;
    missingImagerySurface.kind = RenderCommandKind::GltfPrimitive;
    missingImagerySurface.terrainRenderContent = true;
    missingImagerySurface.terrainSurfaceSource =
        TerrainSurfaceCommandSource::EllipsoidFallback;

    RenderCommand terrainGltfPrimitive;
    terrainGltfPrimitive.kind = RenderCommandKind::GltfPrimitive;
    terrainGltfPrimitive.terrainRenderContent = true;

    RenderCommand contentGltfPrimitive;
    contentGltfPrimitive.kind = RenderCommandKind::GltfPrimitiveInstanced;

    RenderCommandList commands = {
        firstSurface,
        secondSurface,
        missingImagerySurface,
        terrainGltfPrimitive,
        contentGltfPrimitive};

    const SceneRenderCommandDiagnosticsSnapshot snapshot =
        SceneRenderCommandDiagnosticsSnapshot::fromCommands(commands);

    EXPECT_EQ(snapshot.drawCalls, 5);
    EXPECT_EQ(snapshot.renderSurfaceTiles, 4);
    EXPECT_EQ(snapshot.surfaceMeshCount, 4);
    EXPECT_EQ(snapshot.terrainSurfaceTileCommands, 4);
    EXPECT_EQ(snapshot.terrainGltfPrimitiveCommands, 4);
    EXPECT_EQ(snapshot.terrainRenderContentCommands, 4);
    EXPECT_EQ(snapshot.terrainSurfaceRealCommands, 1);
    EXPECT_EQ(snapshot.terrainSurfaceFillProxyCommands, 1);
    EXPECT_EQ(snapshot.terrainSurfaceEllipsoidCommands, 1);
    EXPECT_EQ(snapshot.terrainSurfaceUnknownCommands, 1);
    EXPECT_EQ(snapshot.renderGltfPrimitives, 5);
    EXPECT_EQ(snapshot.gpuTextureCount, 2);
    // exact 的语义是「贴的是本级影像」,不是「有任意纹理」:两条带纹理的命令
    // 一条 delta=0(清晰)、一条 delta=1(退回父级上采样)。
    EXPECT_EQ(snapshot.imageryExactAttachments, 1);
    EXPECT_EQ(snapshot.imageryAncestor1Attachments, 1);
    EXPECT_EQ(snapshot.imageryAncestor2Attachments, 0);
    EXPECT_EQ(snapshot.imageryAncestor3PlusAttachments, 0);
    EXPECT_EQ(snapshot.imageryMissingTiles, 2);
    EXPECT_EQ(snapshot.imageryMinTargetZoom, 3);
    EXPECT_EQ(snapshot.imageryMaxTargetZoom, 7);
    EXPECT_EQ(snapshot.imageryMinTextureZoom, 4);
    EXPECT_EQ(snapshot.imageryMaxTextureZoom, 6);

    Diagnostics diagnostics;
    diagnostics.cachedTextures = 9;
    SceneRenderDiagnostics::addRenderCommands(commands, diagnostics);
    SceneRenderDiagnostics::finalizeRenderCommandFields(diagnostics);
    EXPECT_EQ(diagnostics.drawCalls, snapshot.drawCalls);
    EXPECT_EQ(diagnostics.gpuTextureCount, snapshot.gpuTextureCount);
    EXPECT_EQ(diagnostics.renderSurfaceTiles, snapshot.renderSurfaceTiles);
    EXPECT_EQ(
        diagnostics.renderGltfPrimitives,
        snapshot.renderGltfPrimitives);
    EXPECT_EQ(
        diagnostics.terrainRenderContentCommands,
        snapshot.terrainRenderContentCommands);
    EXPECT_EQ(
        diagnostics.terrainSurfaceTileCommands,
        snapshot.terrainSurfaceTileCommands);
    EXPECT_EQ(
        diagnostics.terrainGltfPrimitiveCommands,
        snapshot.terrainGltfPrimitiveCommands);
    EXPECT_EQ(
        diagnostics.terrainSurfaceRealCommands,
        snapshot.terrainSurfaceRealCommands);
    EXPECT_EQ(
        diagnostics.terrainSurfaceFillProxyCommands,
        snapshot.terrainSurfaceFillProxyCommands);
    EXPECT_EQ(
        diagnostics.terrainSurfaceEllipsoidCommands,
        snapshot.terrainSurfaceEllipsoidCommands);
    EXPECT_EQ(
        diagnostics.terrainSurfaceUnknownCommands,
        snapshot.terrainSurfaceUnknownCommands);
    EXPECT_EQ(diagnostics.imageryAttachments, 2);

    RenderCommandList commandsWithoutTextures = {missingImagerySurface};
    SceneRenderDiagnostics::addRenderCommands(
        commandsWithoutTextures,
        diagnostics);
    SceneRenderDiagnostics::finalizeRenderCommandFields(diagnostics);
    EXPECT_EQ(diagnostics.gpuTextureCount, diagnostics.cachedTextures);
    EXPECT_EQ(diagnostics.imageryMissingTiles, 1);
    EXPECT_EQ(diagnostics.imageryAttachments, 0);
}

TEST(
    SceneRenderCommandDiagnosticsSnapshotTest,
    IgnoresNullTexturesAndNegativeZoomRanges) {
    Texture* texture = reinterpret_cast<Texture*>(0x1);

    RenderCommand invalidZoomSurface;
    invalidZoomSurface.kind = RenderCommandKind::GltfPrimitive;
    invalidZoomSurface.textures = {nullptr, texture, texture};
    invalidZoomSurface.surfaceGeometryZoom = -1;
    invalidZoomSurface.surfaceTextureZoom = -1;
    invalidZoomSurface.imageryAncestorLevelDelta = 0;

    // 非地形、无纹理的 glTF 命令(极帽就是这种):不算「缺影像瓦片」。
    RenderCommand nonTerrainNoTexture;
    nonTerrainNoTexture.kind = RenderCommandKind::GltfPrimitive;
    nonTerrainNoTexture.surfaceGeometryZoom = 5;
    nonTerrainNoTexture.surfaceTextureZoom = 6;

    const SceneRenderCommandDiagnosticsSnapshot snapshot =
        SceneRenderCommandDiagnosticsSnapshot::fromCommands(
            {invalidZoomSurface, nonTerrainNoTexture});

    EXPECT_EQ(snapshot.drawCalls, 2);
    EXPECT_EQ(snapshot.renderSurfaceTiles, 0);
    EXPECT_EQ(snapshot.imageryExactAttachments, 1);
    // 极帽契约:非地形命令没有影像是常态,不是空洞。此前把它计进 missing,
    // 导致调试面板恒显示「2 missing」(两个极帽)从不消失。
    EXPECT_EQ(snapshot.imageryMissingTiles, 0);
    EXPECT_EQ(snapshot.gpuTextureCount, 1);
    EXPECT_EQ(snapshot.imageryMinTargetZoom, 0);
    EXPECT_EQ(snapshot.imageryMaxTargetZoom, 0);
    EXPECT_EQ(snapshot.imageryMinTextureZoom, 0);
    EXPECT_EQ(snapshot.imageryMaxTextureZoom, 0);
}

// 地形合批后一条实例化命令代表 instanceCount 片瓦片。影像直方图是「屏幕上
// 有多少片糊」的口径,必须按瓦片加权,否则掠视下 128 片可见瓦片只数到命令数。
// (绘制开销口径的 drawCalls/terrainSurface* 仍按命令计,不加权。)
TEST(SceneRenderCommandDiagnosticsSnapshotTest, WeightsBatchedTilesByInstance) {
    Texture* texture = reinterpret_cast<Texture*>(0x1);

    RenderCommand batch;
    batch.kind = RenderCommandKind::GltfPrimitiveInstanced;
    batch.terrainRenderContent = true;
    batch.terrainSurfaceSource = TerrainSurfaceCommandSource::RealTerrain;
    batch.textures = {texture};
    batch.instanceCount = 7;
    batch.imageryAncestorLevelDelta = 2;

    RenderCommand single;
    single.kind = RenderCommandKind::GltfPrimitive;
    single.terrainRenderContent = true;
    single.terrainSurfaceSource = TerrainSurfaceCommandSource::RealTerrain;
    single.textures = {texture};
    single.imageryAncestorLevelDelta = 0;

    const SceneRenderCommandDiagnosticsSnapshot snapshot =
        SceneRenderCommandDiagnosticsSnapshot::fromCommands({batch, single});

    EXPECT_EQ(snapshot.imageryAncestor2Attachments, 7);  // 按瓦片
    EXPECT_EQ(snapshot.imageryExactAttachments, 1);
    EXPECT_EQ(snapshot.drawCalls, 2);                    // 按命令
    EXPECT_EQ(snapshot.terrainSurfaceRealCommands, 2);   // 按命令
}

TEST(
    SceneSurfaceCommandGenerationDiagnosticsSnapshotTest,
    TracksSurfaceGenerations) {
    RenderCommand currentSurface;
    currentSurface.kind = RenderCommandKind::GltfPrimitive;
    currentSurface.terrainRenderContent = true;
    currentSurface.frameId = 12;
    currentSurface.generation = 5;

    RenderCommand staleSurface;
    staleSurface.kind = RenderCommandKind::GltfPrimitive;
    staleSurface.terrainRenderContent = true;
    staleSurface.frameId = 11;
    staleSurface.generation = 9;

    RenderCommand missingGenerationSurface;
    missingGenerationSurface.kind = RenderCommandKind::GltfPrimitive;
    missingGenerationSurface.terrainRenderContent = true;
    missingGenerationSurface.frameId = 12;
    missingGenerationSurface.generation = 0;

    RenderCommand gltfPrimitive;
    gltfPrimitive.kind = RenderCommandKind::GltfPrimitiveInstanced;
    gltfPrimitive.frameId = 10;
    gltfPrimitive.generation = 2;

    const RenderCommandList commands = {
        currentSurface,
        staleSurface,
        missingGenerationSurface,
        gltfPrimitive};

    const SceneSurfaceCommandGenerationDiagnosticsSnapshot snapshot =
        SceneSurfaceCommandGenerationDiagnosticsSnapshot::fromCommands(
            commands,
            12);
    EXPECT_EQ(snapshot.staleSurfaceCommands, 1);
    EXPECT_EQ(snapshot.missingGenerationSurfaceCommands, 1);
    EXPECT_EQ(snapshot.minSurfaceGeneration, 5u);
    EXPECT_EQ(snapshot.maxSurfaceGeneration, 9u);

    Diagnostics diagnostics;
    diagnostics.staleSurfaceCommands = 7;
    diagnostics.missingGenerationSurfaceCommands = 8;
    diagnostics.minSurfaceGeneration = 99;
    diagnostics.maxSurfaceGeneration = 100;
    SceneRenderDiagnostics::updateSurfaceCommandGeneration(
        commands,
        12,
        diagnostics);
    EXPECT_EQ(
        diagnostics.staleSurfaceCommands,
        snapshot.staleSurfaceCommands);
    EXPECT_EQ(
        diagnostics.missingGenerationSurfaceCommands,
        snapshot.missingGenerationSurfaceCommands);
    EXPECT_EQ(
        diagnostics.minSurfaceGeneration,
        snapshot.minSurfaceGeneration);
    EXPECT_EQ(
        diagnostics.maxSurfaceGeneration,
        snapshot.maxSurfaceGeneration);

    SceneRenderDiagnostics::updateSurfaceCommandGeneration(
        RenderCommandList{},
        12,
        diagnostics);
    EXPECT_EQ(diagnostics.staleSurfaceCommands, 0);
    EXPECT_EQ(diagnostics.missingGenerationSurfaceCommands, 0);
    EXPECT_EQ(diagnostics.minSurfaceGeneration, 0u);
    EXPECT_EQ(diagnostics.maxSurfaceGeneration, 0u);
}
