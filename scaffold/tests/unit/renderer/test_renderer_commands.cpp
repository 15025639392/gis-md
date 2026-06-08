#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"

using namespace earth_engine;

TEST(RendererCommandTest, SurfaceTilesAreAuthoritativeDepthSurface) {
    Renderer renderer(nullptr);

    RenderCommand cmd = renderer.makeSurfaceTileCommand(
        nullptr,
        nullptr,
        nullptr,
        42);

    EXPECT_EQ(RenderCommandKind::SurfaceTile, cmd.kind);
    EXPECT_EQ("surface_tile", cmd.owner);
    EXPECT_EQ(42, cmd.indexCount);
    EXPECT_EQ(32, cmd.vertexStride);
    EXPECT_EQ(0u, cmd.frameId);
    EXPECT_EQ(0u, cmd.generation);
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_TRUE(cmd.depthWrite);
    EXPECT_TRUE(cmd.cullFace);
    EXPECT_FALSE(cmd.blend);
}

TEST(RendererCommandTest, MvpValidatorAcceptsSurfaceTileAsSurface) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = false;
    tile.generation = 1;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, MvpValidatorRejectsMutableSurfaceTileDepthAndCullState) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = false;
    tile.depthWrite = false;
    tile.cullFace = false;
    tile.blend = true;
    tile.generation = 1;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("surface_tile", error->owner);
}

TEST(RendererCommandTest, MvpValidatorRejectsStaleSurfaceTileFrameId) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = false;
    tile.frameId = 41;
    tile.generation = 7;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands, 42);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("surface_tile", error->owner);
}

TEST(RendererCommandTest, MvpValidatorRejectsMissingSurfaceTileGeneration) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = false;
    tile.frameId = 42;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands, 42);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("surface_tile", error->owner);
}

TEST(RendererCommandTest, MvpSortEnforcesSurfaceVectorDebugOrder) {
    RenderCommand debug;
    debug.kind = RenderCommandKind::DebugOverlay;
    debug.owner = "debug";
    debug.pass = "color";

    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = false;
    tile.generation = 1;

    RenderCommand globe;
    globe.kind = RenderCommandKind::GlobeSurface;
    globe.owner = "globe";
    globe.pass = "color";
    globe.depthTest = true;
    globe.depthWrite = true;
    globe.cullFace = true;
    globe.blend = false;

    RenderCommand vector;
    vector.kind = RenderCommandKind::VectorOverlay;
    vector.owner = "vector";
    vector.pass = "color";

    RenderCommandList commands{debug, tile, vector, globe};
    sortMvpRenderCommands(commands);

    EXPECT_EQ(10, mvpRenderOrder(commands[0].kind));
    EXPECT_EQ(10, mvpRenderOrder(commands[1].kind));
    EXPECT_EQ(RenderCommandKind::VectorOverlay, commands[2].kind);
    EXPECT_EQ(RenderCommandKind::DebugOverlay, commands[3].kind);
    EXPECT_FALSE(validateMvpRenderCommands(commands).has_value());
}
