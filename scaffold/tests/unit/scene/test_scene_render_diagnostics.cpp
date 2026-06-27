#include <gtest/gtest.h>

#include "earth_engine/scene/SceneRenderDiagnostics.h"

using namespace earth_engine;

TEST(SceneRenderCommandDiagnosticsSnapshotTest, CountsRenderCommandLanes) {
    Texture* sharedTexture = reinterpret_cast<Texture*>(0x1);
    Texture* secondTexture = reinterpret_cast<Texture*>(0x2);

    RenderCommand firstSurface;
    firstSurface.kind = RenderCommandKind::GltfPrimitive;
    firstSurface.terrainRenderContent = true;
    firstSurface.textures = {sharedTexture};
    firstSurface.surfaceGeometryZoom = 3;
    firstSurface.surfaceTextureZoom = 4;

    RenderCommand secondSurface;
    secondSurface.kind = RenderCommandKind::GltfPrimitive;
    secondSurface.terrainRenderContent = true;
    secondSurface.textures = {sharedTexture, secondTexture};
    secondSurface.surfaceGeometryZoom = 7;
    secondSurface.surfaceTextureZoom = 6;

    RenderCommand missingImagerySurface;
    missingImagerySurface.kind = RenderCommandKind::GltfPrimitive;
    missingImagerySurface.terrainRenderContent = true;

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
    EXPECT_EQ(snapshot.renderGltfPrimitives, 5);
    EXPECT_EQ(snapshot.gpuTextureCount, 2);
    EXPECT_EQ(snapshot.imageryExactAttachments, 2);
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

    RenderCommand missingImagerySurface;
    missingImagerySurface.kind = RenderCommandKind::GltfPrimitive;
    missingImagerySurface.surfaceGeometryZoom = 5;
    missingImagerySurface.surfaceTextureZoom = 6;

    const SceneRenderCommandDiagnosticsSnapshot snapshot =
        SceneRenderCommandDiagnosticsSnapshot::fromCommands(
            {invalidZoomSurface, missingImagerySurface});

    EXPECT_EQ(snapshot.drawCalls, 2);
    EXPECT_EQ(snapshot.renderSurfaceTiles, 0);
    EXPECT_EQ(snapshot.imageryExactAttachments, 1);
    EXPECT_EQ(snapshot.imageryMissingTiles, 1);
    EXPECT_EQ(snapshot.gpuTextureCount, 1);
    EXPECT_EQ(snapshot.imageryMinTargetZoom, 0);
    EXPECT_EQ(snapshot.imageryMaxTargetZoom, 0);
    EXPECT_EQ(snapshot.imageryMinTextureZoom, 0);
    EXPECT_EQ(snapshot.imageryMaxTextureZoom, 0);
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
