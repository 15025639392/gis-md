#include <gtest/gtest.h>

#include "earth_engine/renderer/TerrainDepthPrepass.h"
#include "earth_engine/renderer/Renderer.h"
#include "../../helpers/MockRenderDevice.h"

using namespace earth_engine;

namespace {

RenderCommand makeCommand(RenderCommandKind kind,
                          TerrainSurfaceCommandSource source) {
    RenderCommand cmd;
    cmd.kind = kind;
    cmd.terrainSurfaceSource = source;
    cmd.pass = "color";
    cmd.blend = true;
    cmd.depthWrite = false;
    return cmd;
}

} // namespace

// device/shader 未接线时整条通路 no-op —— 这是 Metal 侧与所有 host 测试走的
// 路径,必须保证它不产生任何命令(而不是产生一批没有 shader 的空命令)。
TEST(TerrainDepthPrepassTest, NotReadyProducesNoCommands) {
    TerrainDepthPrepass prepass;
    EXPECT_FALSE(prepass.ready());
    EXPECT_EQ(nullptr, prepass.depthTexture());
    EXPECT_EQ(nullptr, prepass.ensureFramebuffer(1024, 768));

    RenderCommandList scene;
    scene.push_back(makeCommand(RenderCommandKind::GltfPrimitive,
                                TerrainSurfaceCommandSource::RealTerrain));
    EXPECT_TRUE(prepass.extractTerrainCommands(scene).empty());
}

// 半分辨率是刻意的:符号遮挡是「锚点在山前还是山后」的二元判定,不需要全
// 分辨率;这条钉住那个约定,免得后人"顺手"改成全分辨率而不知道代价。
TEST(TerrainDepthPrepassTest, ResolutionDivisorIsHalf) {
    EXPECT_EQ(2, TerrainDepthPrepass::kResolutionDivisor);
}

TEST(TerrainDepthPrepassTest, ReadyAcceptsAnyAvailableDepthShaderPath) {
    earth_engine::testing::MockRenderDevice device;
    Renderer renderer(&device);
    ASSERT_TRUE(renderer.initialize());

    TerrainDepthPrepass prepass;
    ASSERT_TRUE(prepass.initialize(&device, &renderer));
    EXPECT_TRUE(prepass.ready());
}

TEST(TerrainDepthPrepassTest,
     ExtractsStableTerrainAndEllipsoidWithMatchingVertexLayouts) {
    earth_engine::testing::MockRenderDevice device;
    Renderer renderer(&device);
    ASSERT_TRUE(renderer.initialize());

    TerrainDepthPrepass prepass;
    ASSERT_TRUE(prepass.initialize(&device, &renderer));

    RenderCommand terrain32 = makeCommand(
        RenderCommandKind::GltfPrimitive,
        TerrainSurfaceCommandSource::RealTerrain);
    terrain32.vertexStride = 32;
    RenderCommand ellipsoid120 = makeCommand(
        RenderCommandKind::GltfPrimitive,
        TerrainSurfaceCommandSource::EllipsoidFallback);
    ellipsoid120.vertexStride = 120;
    RenderCommand fillProxy = makeCommand(
        RenderCommandKind::GltfPrimitive,
        TerrainSurfaceCommandSource::FillProxy);
    fillProxy.vertexStride = 120;

    const RenderCommandList depth = prepass.extractTerrainCommands(
        {terrain32, ellipsoid120, fillProxy});
    ASSERT_EQ(2u, depth.size());
    EXPECT_EQ(renderer.terrainDepthShader(), depth[0].shader);
    EXPECT_EQ(renderer.gltfDepthShader(), depth[1].shader);
    EXPECT_EQ("depth", depth[0].pass);
    EXPECT_EQ("depth", depth[1].pass);
    EXPECT_FALSE(depth[0].blend);
    EXPECT_TRUE(depth[0].depthTest);
    EXPECT_TRUE(depth[0].depthWrite);
}

TEST(TerrainDepthPrepassTest, RejectsPartialDepthWhenLayoutIsUnsupported) {
    earth_engine::testing::MockRenderDevice device;
    Renderer renderer(&device);
    ASSERT_TRUE(renderer.initialize());
    TerrainDepthPrepass prepass;
    ASSERT_TRUE(prepass.initialize(&device, &renderer));

    RenderCommand supported = makeCommand(
        RenderCommandKind::GltfPrimitive,
        TerrainSurfaceCommandSource::EllipsoidFallback);
    supported.vertexStride = 120;
    RenderCommand unsupported = makeCommand(
        RenderCommandKind::GltfPrimitive,
        TerrainSurfaceCommandSource::RealTerrain);
    unsupported.vertexStride = 48;

    EXPECT_TRUE(prepass.extractTerrainCommands(
        {supported, unsupported}).empty());
}
