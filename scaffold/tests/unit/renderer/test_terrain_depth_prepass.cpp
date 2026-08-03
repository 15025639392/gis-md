#include <gtest/gtest.h>

#include "earth_engine/renderer/TerrainDepthPrepass.h"

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
