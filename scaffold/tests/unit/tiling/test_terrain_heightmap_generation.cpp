#include <gtest/gtest.h>

#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/tiling/TileRenderContentState.h"

#include <memory>

using namespace earth_engine;

// === heightmapGeneration 收口契约守卫 ===
//
// 契约:surface_.heightmap 的每一个变异点(赋值/reset)都必须在"真的有变化"
// 时 bump 进程级 heightmapGeneration,且稳态(无变化)读数必须纹丝不动。
// 消费方(高度索引重建/相机探针失效/矢量重钳节流)全部依赖这一信号,漏 bump
// = 静默用 stale 高度。当前变异点共 5 处,全部在 TileRenderContentState 类内:
//   1. setRetainedHeightmap 主路径(置入/替换/置空)
//   2. setRetainedHeightmap gltf-owned 清理分支
//   3. clearRetainedHeightmap
//   4. clearRenderContent
//   5. prepareGltfContent / clearGltfContent(经 clearGltfContentState)
// 新增变异点时必须同步扩这里的用例——grep `surface_.heightmap` 的写点数
// 应与本文件覆盖的路径一一对应。

namespace {

std::unique_ptr<DecodedHeightmap> makeHeightmap(float height) {
    auto heightmap = std::make_unique<DecodedHeightmap>();
    heightmap->tileSize = 2;
    heightmap->heights = {height, height, height, height};
    heightmap->minHeight = height;
    heightmap->maxHeight = height;
    return heightmap;
}

std::uint64_t gen() { return TileRenderContentState::heightmapGeneration(); }

} // namespace

TEST(TerrainHeightmapGenerationTest, SetBumpsAndReplaceBumps) {
    TileRenderContentState state;

    const std::uint64_t g0 = gen();
    state.setRetainedHeightmap(makeHeightmap(10.0f));
    const std::uint64_t g1 = gen();
    EXPECT_GT(g1, g0) << "置入 heightmap 必须 bump";

    state.setRetainedHeightmap(makeHeightmap(20.0f));
    const std::uint64_t g2 = gen();
    EXPECT_GT(g2, g1) << "替换 heightmap 必须 bump";
}

TEST(TerrainHeightmapGenerationTest, NoopSetDoesNotBump) {
    TileRenderContentState state;

    // 无 heightmap 时 set(nullptr) 是纯 no-op:稳态不许有噪声,否则消费方
    // 每帧都被假信号打醒(E5 判据:稳态 bump/帧 == 0)。
    const std::uint64_t g0 = gen();
    state.setRetainedHeightmap(nullptr);
    EXPECT_EQ(gen(), g0) << "无变化的 set(nullptr) 不许 bump";

    state.clearRetainedHeightmap();
    EXPECT_EQ(gen(), g0) << "无 heightmap 时 clear 不许 bump";

    state.clearRenderContent();
    EXPECT_EQ(gen(), g0) << "无 heightmap 时 clearRenderContent 不许 bump";
}

TEST(TerrainHeightmapGenerationTest, ClearRetainedHeightmapBumps) {
    TileRenderContentState state;
    state.setRetainedHeightmap(makeHeightmap(10.0f));

    const std::uint64_t g0 = gen();
    state.clearRetainedHeightmap();
    EXPECT_GT(gen(), g0);
}

TEST(TerrainHeightmapGenerationTest, ClearRenderContentBumps) {
    TileRenderContentState state;
    state.setTerrainRenderContent(true);
    state.setRetainedHeightmap(makeHeightmap(10.0f));

    const std::uint64_t g0 = gen();
    state.clearRenderContent();
    EXPECT_GT(gen(), g0) << "unload 收口消亡 heightmap 必须 bump";
}

TEST(TerrainHeightmapGenerationTest, PrepareGltfContentBumpsWhenHeightmapDies) {
    TileRenderContentState state;
    state.setRetainedHeightmap(makeHeightmap(10.0f));

    const std::uint64_t g0 = gen();
    state.prepareGltfContent(std::make_unique<GltfModel>(), Mat4::identity());
    EXPECT_GT(gen(), g0) << "glTF 接管静默 reset heightmap 必须 bump";

    // 没有 heightmap 的再次接管不许 bump。
    const std::uint64_t g1 = gen();
    state.prepareGltfContent(std::make_unique<GltfModel>(), Mat4::identity());
    EXPECT_EQ(gen(), g1);
}

TEST(TerrainHeightmapGenerationTest, GltfOwnedCleanupBranchBumps) {
    TileRenderContentState state;
    // Phase 2c 形态:heightmap 地形当 glTF 交付 → gltf-owned 且显式保留高度图。
    state.prepareGltfContent(std::make_unique<GltfModel>(), Mat4::identity());
    state.setRetainedHeightmap(makeHeightmap(10.0f));

    // gltf-owned 态 set(nullptr) 走清理分支,heightmap 从有到无必须 bump。
    const std::uint64_t g0 = gen();
    state.setRetainedHeightmap(nullptr);
    EXPECT_GT(gen(), g0);
}

TEST(TerrainHeightmapGenerationTest, ClearGltfContentBumpsWhenHeightmapDies) {
    TileRenderContentState state;
    state.prepareGltfContent(std::make_unique<GltfModel>(), Mat4::identity());
    state.setRetainedHeightmap(makeHeightmap(10.0f));

    const std::uint64_t g0 = gen();
    state.clearGltfContent();
    EXPECT_GT(gen(), g0) << "gltf-owned 清理连带消亡 heightmap 必须 bump";
}
