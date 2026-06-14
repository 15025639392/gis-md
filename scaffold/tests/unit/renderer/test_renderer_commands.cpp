#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"

using namespace earth_engine;

namespace {

class DummyTexture final : public Texture {
public:
    explicit DummyTexture(int id) : id_(id) {}
    int width() const override { return 256; }
    int height() const override { return 256; }
    int id() const { return id_; }

private:
    int id_ = 0;
};

} // namespace

TEST(RendererCommandTest, SurfaceTilesAreAuthoritativeDepthSurface) {
    Renderer renderer(nullptr);

    RenderCommand cmd = renderer.makeSurfaceTileCommand(
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        42);

    EXPECT_EQ(RenderCommandKind::SurfaceTile, cmd.kind);
    EXPECT_EQ("surface_tile", cmd.owner);
    EXPECT_EQ(42, cmd.indexCount);
    EXPECT_EQ(20, cmd.vertexStride);
    EXPECT_EQ(0u, cmd.frameId);
    EXPECT_EQ(0u, cmd.generation);
    EXPECT_TRUE(cmd.hasSurfaceTileUniforms);
    EXPECT_EQ(1.0f, cmd.surfaceTileOpacity);
    EXPECT_EQ(1.0f, cmd.surfaceTransitionOpacity);
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_TRUE(cmd.depthWrite);
    EXPECT_TRUE(cmd.cullFace);
    EXPECT_FALSE(cmd.blend);
}

TEST(RendererCommandTest, SurfaceTileImageryTextureSlotsAreStable) {
    Renderer renderer(nullptr);
    DummyTexture base(0);
    DummyTexture water(5);
    DummyTexture overlays[] = {
        DummyTexture(1),
        DummyTexture(2),
        DummyTexture(3),
        DummyTexture(4),
        DummyTexture(99)
    };

    RenderCommand cmd = renderer.makeSurfaceTileCommand(
        &base,
        &water,
        nullptr,
        nullptr,
        42);

    ASSERT_EQ(6u, cmd.textures.size());
    EXPECT_EQ(&base, cmd.textures[0]);
    EXPECT_EQ(nullptr, cmd.textures[1]);
    EXPECT_EQ(nullptr, cmd.textures[2]);
    EXPECT_EQ(nullptr, cmd.textures[3]);
    EXPECT_EQ(nullptr, cmd.textures[4]);
    EXPECT_EQ(&water, cmd.textures[5]);
    EXPECT_EQ(1.0f, cmd.surfaceHasWaterMask);

    for (int i = 0; i < kMaxSurfaceImageryOverlays; ++i) {
        EXPECT_TRUE(renderer.attachSurfaceOverlayTexture(
            cmd,
            &overlays[i],
            0.1f * static_cast<float>(i),
            0.2f * static_cast<float>(i),
            0.3f,
            0.4f,
            0.5f));
    }
    EXPECT_FALSE(renderer.attachSurfaceOverlayTexture(
        cmd,
        &overlays[4],
        0.0f,
        0.0f,
        1.0f,
        1.0f,
        1.0f));

    ASSERT_EQ(6u, cmd.textures.size());
    EXPECT_EQ(&base, cmd.textures[0]);
    EXPECT_EQ(&overlays[0], cmd.textures[1]);
    EXPECT_EQ(&overlays[1], cmd.textures[2]);
    EXPECT_EQ(&overlays[2], cmd.textures[3]);
    EXPECT_EQ(&overlays[3], cmd.textures[4]);
    EXPECT_EQ(&water, cmd.textures[5]);
    EXPECT_EQ(kMaxSurfaceImageryOverlays, cmd.surfaceOverlayTextureCount);
    EXPECT_FLOAT_EQ(0.5f, cmd.surfaceOverlayOpacities[3]);
    EXPECT_FLOAT_EQ(0.3f, cmd.surfaceOverlayTileUvs[3][2]);
}

TEST(RendererCommandTest, InstancedSurfaceTileUsesSharedGridAndInstanceBuffer) {
    Renderer renderer(nullptr);

    RenderCommand cmd = renderer.makeInstancedSurfaceTileCommand(
        nullptr,
        nullptr,
        7);

    EXPECT_EQ(RenderCommandKind::SurfaceTile, cmd.kind);
    EXPECT_EQ("surface_tile", cmd.owner);
    EXPECT_EQ(8, cmd.vertexStride);
    EXPECT_EQ(7, cmd.instanceCount);
    EXPECT_EQ(120, cmd.instanceStride);
    EXPECT_TRUE(cmd.hasSurfaceTileUniforms);
    EXPECT_TRUE(cmd.depthTest);
    EXPECT_TRUE(cmd.depthWrite);
    EXPECT_TRUE(cmd.cullFace);
}

TEST(RendererCommandTest, SurfaceTileBlendAllowedForLodTransitionOpacity) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = true;
    tile.generation = 1;
    tile.hasSurfaceTileUniforms = true;
    tile.surfaceTransitionOpacity = 0.5f;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands);
    EXPECT_FALSE(error.has_value());
}

TEST(RendererCommandTest, SurfaceTileBlendRejectedWithoutOpacityReason) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "surface_tile";
    tile.pass = "color";
    tile.depthTest = true;
    tile.depthWrite = true;
    tile.cullFace = true;
    tile.blend = true;
    tile.generation = 1;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ("surface_tile", error->owner);
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

TEST(RendererCommandTest, MvpValidatorAcceptsTerrainPrimaryOverlayDepthState) {
    RenderCommand tile;
    tile.kind = RenderCommandKind::SurfaceTile;
    tile.owner = "terrain_primary_surface";
    tile.pass = "color";
    tile.depthTest = false;
    tile.depthWrite = false;
    tile.cullFace = false;
    tile.blend = false;
    tile.frameId = 42;
    tile.generation = 7;
    tile.hasSurfaceTileUniforms = true;

    RenderCommandList commands{tile};
    auto error = validateMvpRenderCommands(commands, 42);
    EXPECT_FALSE(error.has_value());
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
