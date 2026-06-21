#include <gtest/gtest.h>

#include "earth_engine/scene/SceneRenderDiagnostics.h"

using namespace earth_engine;

TEST(SceneRenderCommandDiagnosticsSnapshotTest, CountsRenderCommandLanes) {
    Texture* sharedTexture = reinterpret_cast<Texture*>(0x1);
    Texture* secondTexture = reinterpret_cast<Texture*>(0x2);

    RenderCommand firstSurface;
    firstSurface.kind = RenderCommandKind::SurfaceTile;
    firstSurface.textures = {sharedTexture};
    firstSurface.surfaceGeometryZoom = 3;
    firstSurface.surfaceTextureZoom = 4;

    RenderCommand secondSurface;
    secondSurface.kind = RenderCommandKind::SurfaceTile;
    secondSurface.textures = {sharedTexture, secondTexture};
    secondSurface.surfaceGeometryZoom = 7;
    secondSurface.surfaceTextureZoom = 6;

    RenderCommand missingImagerySurface;
    missingImagerySurface.kind = RenderCommandKind::SurfaceTile;

    RenderCommand gltfPrimitive;
    gltfPrimitive.kind = RenderCommandKind::GltfPrimitive;

    RenderCommand instancedGltfPrimitive;
    instancedGltfPrimitive.kind = RenderCommandKind::GltfPrimitiveInstanced;

    RenderCommandList commands = {
        firstSurface,
        secondSurface,
        missingImagerySurface,
        gltfPrimitive,
        instancedGltfPrimitive};

    const SceneRenderCommandDiagnosticsSnapshot snapshot =
        SceneRenderCommandDiagnosticsSnapshot::fromCommands(commands);

    EXPECT_EQ(snapshot.drawCalls, 5);
    EXPECT_EQ(snapshot.renderSurfaceTiles, 3);
    EXPECT_EQ(snapshot.surfaceMeshCount, 3);
    EXPECT_EQ(snapshot.terrainSurfaceMeshes, 3);
    EXPECT_EQ(snapshot.terrainReadySurfaceMeshes, 3);
    EXPECT_EQ(snapshot.renderGltfPrimitives, 2);
    EXPECT_EQ(snapshot.gpuTextureCount, 2);
    EXPECT_EQ(snapshot.imageryExactAttachments, 2);
    EXPECT_EQ(snapshot.imageryMissingTiles, 1);
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
