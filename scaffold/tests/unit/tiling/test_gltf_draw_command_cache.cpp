#include <gtest/gtest.h>

#include "earth_engine/renderer/Renderer.h"
#include "earth_engine/tiling/GltfDrawCommandBuilder.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/layers/ActivatedRasterOverlay.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/math/Mat4.h"

#include "../../helpers/MockRenderDevice.h"

#include <array>
#include <memory>
#include <vector>

using namespace earth_engine;

// P0-4: GltfDrawCommandBuilder 的 per-tile 常驻命令缓存语义。
// 内容不变式(几何/材质/stableKey)只在缓存失效时重建;每帧字段
// (frameId/opacity/blend 派生/clip)盖在帧列表副本上,不污染常驻命令。

namespace {

GltfPrimitiveRenderResources makePrimitive(RenderDevice& device) {
    GltfPrimitiveRenderResources primitive;
    BufferDesc vbDesc;
    vbDesc.size = 32 * 3;
    vbDesc.type = BufferDesc::Type::Vertex;
    primitive.vertexBuffer = device.createBuffer(vbDesc);
    BufferDesc ibDesc;
    ibDesc.size = sizeof(uint32_t) * 3;
    ibDesc.type = BufferDesc::Type::Index;
    primitive.indexBuffer = device.createBuffer(ibDesc);
    primitive.indexCount = 3;
    primitive.vertexCount = 3;
    primitive.useTerrainVertexFormat = true;
    return primitive;
}

struct CacheHarness {
    earth_engine::testing::MockRenderDevice device;
    Renderer renderer{&device};
    TilesetTile tile{
        TileKey{"Geographic-TMS", 2, 1, 1},
        Rectangle::fromDegrees(-10.0, -5.0, 2.0, 7.0)};
    std::vector<ActivatedRasterOverlay*> overlays;

    CacheHarness() {
        EXPECT_TRUE(renderer.initialize());
        tile.content.renderContent.prepareGltfContent(
            std::make_unique<GltfModel>(), Mat4::identity());
        tile.content.renderContent.setTerrainRenderContent(true);
        tile.content.renderContent.addGltfPrimitiveResource(
            makePrimitive(device));
        tile.content.renderContent.setGltfResourcesReady(true);
    }

    RenderCommandList buildFrame(const GltfDrawCommandBuildContext& context) {
        RenderCommandList commands;
        GltfDrawCommandBuilder::build(
            renderer, tile, overlays, commands, context);
        return commands;
    }
};

} // namespace

TEST(GltfDrawCommandCacheTest, BuildPopulatesResidentCacheWithStableKey) {
    CacheHarness harness;
    EXPECT_FALSE(harness.tile.content.renderContent.hasCachedDrawCommands());

    GltfDrawCommandBuildContext context;
    context.frameNumber = 7;
    context.generation = 3;
    RenderCommandList commands = harness.buildFrame(context);

    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ("Geographic-TMS/2/1/1#0", commands[0].stableKey);
    EXPECT_EQ(7u, commands[0].frameId);
    EXPECT_EQ(3u, commands[0].generation);
    EXPECT_TRUE(commands[0].terrainRenderContent);
    EXPECT_EQ(
        TerrainSurfaceCommandSource::RealTerrain,
        commands[0].terrainSurfaceSource);
    EXPECT_TRUE(harness.tile.content.renderContent.hasCachedDrawCommands());
    ASSERT_EQ(
        1u, harness.tile.content.renderContent.cachedDrawCommands().size());
}

TEST(GltfDrawCommandCacheTest, ReusesCachedCommandsAcrossFrames) {
    CacheHarness harness;
    GltfDrawCommandBuildContext context;
    context.frameNumber = 1;
    harness.buildFrame(context);

    // 改 tile.key 后不失效重建:第二帧命令仍携带首帧生成的 stableKey,
    // 证明命令来自常驻缓存而非每帧从零重建。
    harness.tile.key = TileKey{"Geographic-TMS", 3, 5, 5};
    context.frameNumber = 2;
    RenderCommandList commands = harness.buildFrame(context);
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ("Geographic-TMS/2/1/1#0", commands[0].stableKey);
    EXPECT_EQ(2u, commands[0].frameId);

    // 显式失效后重建才拾取新 key。
    harness.tile.content.renderContent.invalidateCachedDrawCommands();
    context.frameNumber = 3;
    commands = harness.buildFrame(context);
    ASSERT_EQ(1u, commands.size());
    EXPECT_EQ("Geographic-TMS/3/5/5#0", commands[0].stableKey);
}

TEST(GltfDrawCommandCacheTest, ResourceMutationInvalidatesCache) {
    CacheHarness harness;
    GltfDrawCommandBuildContext context;
    context.frameNumber = 1;
    harness.buildFrame(context);
    EXPECT_TRUE(harness.tile.content.renderContent.hasCachedDrawCommands());

    harness.tile.content.renderContent.addGltfPrimitiveResource(
        makePrimitive(harness.device));
    EXPECT_FALSE(harness.tile.content.renderContent.hasCachedDrawCommands());

    context.frameNumber = 2;
    RenderCommandList commands = harness.buildFrame(context);
    ASSERT_EQ(2u, commands.size());
    EXPECT_EQ("Geographic-TMS/2/1/1#0", commands[0].stableKey);
    EXPECT_EQ("Geographic-TMS/2/1/1#1", commands[1].stableKey);
}

TEST(GltfDrawCommandCacheTest, BlendStateRederivedEachFrame) {
    CacheHarness harness;

    // geomorph 契约:地形走几何 morph 单层过渡,过渡期**保持不透明**(blend=false,
    // depthWrite=true, renderOpacity=1),不走 cross-fade 的 alpha 双层混合。每帧
    // 重派生的过渡量是 geomorphUpFactor.w(morphFactor),不再是 renderOpacity。
    GltfDrawCommandBuildContext fading;
    fading.frameNumber = 1;
    fading.transitionOpacity = 0.5f;
    RenderCommandList commands = harness.buildFrame(fading);
    ASSERT_EQ(1u, commands.size());
    EXPECT_FALSE(commands[0].blend);
    EXPECT_TRUE(commands[0].depthWrite);
    EXPECT_FLOAT_EQ(1.0f, commands[0].gltfUniforms.renderOpacity);
    EXPECT_FLOAT_EQ(0.5f, commands[0].gltfUniforms.geomorphUpFactor[3]);

    // fade 结束:同一常驻命令的 morphFactor 必须回到 1(不 morph),不能被上帧污染。
    GltfDrawCommandBuildContext opaque;
    opaque.frameNumber = 2;
    opaque.transitionOpacity = 1.0f;
    commands = harness.buildFrame(opaque);
    ASSERT_EQ(1u, commands.size());
    EXPECT_FALSE(commands[0].blend);
    EXPECT_TRUE(commands[0].depthWrite);
    EXPECT_FLOAT_EQ(1.0f, commands[0].gltfUniforms.renderOpacity);
    EXPECT_FLOAT_EQ(1.0f, commands[0].gltfUniforms.geomorphUpFactor[3]);
}

TEST(GltfDrawCommandCacheTest, ClipWindowStampedPerFrameWithoutPollution) {
    CacheHarness harness;

    GltfDrawCommandBuildContext clipped;
    clipped.frameNumber = 1;
    clipped.surfaceClipUv = std::array<float, 4>{0.25f, 0.25f, 0.5f, 0.5f};
    RenderCommandList commands = harness.buildFrame(clipped);
    ASSERT_EQ(1u, commands.size());
    EXPECT_FLOAT_EQ(1.0f, commands[0].gltfUniforms.clipEnabled);
    EXPECT_FLOAT_EQ(1.0f, commands[0].surfaceClipEnabled);
    EXPECT_EQ((std::array<float, 4>{0.25f, 0.25f, 0.5f, 0.5f}),
              commands[0].gltfUniforms.clipUv);

    // 常驻命令保持 clip 关闭:下一帧无 clip 的实例继承默认态。
    const RenderCommandList& cached =
        harness.tile.content.renderContent.cachedDrawCommands();
    ASSERT_EQ(1u, cached.size());
    EXPECT_FLOAT_EQ(0.0f, cached[0].gltfUniforms.clipEnabled);

    GltfDrawCommandBuildContext unclipped;
    unclipped.frameNumber = 2;
    commands = harness.buildFrame(unclipped);
    ASSERT_EQ(1u, commands.size());
    EXPECT_FLOAT_EQ(0.0f, commands[0].gltfUniforms.clipEnabled);
    EXPECT_FLOAT_EQ(0.0f, commands[0].surfaceClipEnabled);
    EXPECT_EQ((std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f}),
              commands[0].gltfUniforms.clipUv);
}

TEST(GltfDrawCommandCacheTest, ContentClearDropsCache) {
    CacheHarness harness;
    GltfDrawCommandBuildContext context;
    context.frameNumber = 1;
    harness.buildFrame(context);
    EXPECT_TRUE(harness.tile.content.renderContent.hasCachedDrawCommands());

    harness.tile.content.renderContent.releaseGpuResources();
    EXPECT_FALSE(harness.tile.content.renderContent.hasCachedDrawCommands());
    EXPECT_TRUE(
        harness.tile.content.renderContent.cachedDrawCommands().empty());

    // 资源没了:build 直接返回空,不触碰失效缓存。
    context.frameNumber = 2;
    RenderCommandList commands = harness.buildFrame(context);
    EXPECT_TRUE(commands.empty());
    EXPECT_FALSE(harness.tile.content.renderContent.hasCachedDrawCommands());
}
