#include <gtest/gtest.h>

#include "earth_engine/renderer/TileCompositeBakePoc.h"
#include "../../helpers/MockRenderDevice.h"

using namespace earth_engine;
using earth_engine::testing::MockRenderDevice;

namespace {

TileCompositeBakePocConfig cfg() {
    TileCompositeBakePocConfig c;
    c.bakeTargetSize = 128;
    c.maxBakeTilesPerFrame = 64;
    c.quadsPerTile = 0;
    return c;
}

}  // namespace

TEST(TileCompositeBakePoc, InitializeAndEnsureBuildsBakeFbo) {
    MockRenderDevice device;
    TileCompositeBakePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    EXPECT_TRUE(poc.ensureResources());
    EXPECT_TRUE(poc.isReady());
    EXPECT_EQ(device.createdFramebufferCount, 1);
}

TEST(TileCompositeBakePoc, InitFailsOnNullDevice) {
    TileCompositeBakePoc poc;
    EXPECT_FALSE(poc.initialize(nullptr, cfg()));
}

TEST(TileCompositeBakePoc, TickRunsOnePassPerTileAndRecordsStats) {
    MockRenderDevice device;
    TileCompositeBakePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    ASSERT_TRUE(poc.ensureResources());

    const TileCompositeBakeFrameStats s = poc.tick(10);
    EXPECT_TRUE(s.ready);
    EXPECT_EQ(s.bakedTiles, 10);
    EXPECT_EQ(device.beginPassCount, 10);  // N 个离屏 bake pass
    EXPECT_EQ(device.endPassCount, 10);
    // B 逐瓦片纹理内存 = N × 128² × 4。
    EXPECT_EQ(s.bakeBytes, 10LL * 128 * 128 * 4);
    EXPECT_GE(s.bakeMs, 0.0);
}

TEST(TileCompositeBakePoc, TileCountCappedButMemoryReflectsTrueDemand) {
    MockRenderDevice device;
    TileCompositeBakePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));  // cap = 64
    ASSERT_TRUE(poc.ensureResources());

    const TileCompositeBakeFrameStats s = poc.tick(200);  // 超上限
    EXPECT_EQ(s.bakedTiles, 64);            // pass 数截到上限
    EXPECT_EQ(device.beginPassCount, 64);
    // 内存需求按真实 200 算(反映场景真实需要,不被上限掩盖)。
    EXPECT_EQ(s.bakeBytes, 200LL * 128 * 128 * 4);
}

TEST(TileCompositeBakePoc, TickBeforeReadyIsNoop) {
    MockRenderDevice device;
    TileCompositeBakePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    // 未 ensureResources → 无 FBO → tick 空转。
    const TileCompositeBakeFrameStats s = poc.tick(10);
    EXPECT_FALSE(s.ready);
    EXPECT_EQ(device.beginPassCount, 0);
}

TEST(TileCompositeBakePoc, ZeroTilesNoPasses) {
    MockRenderDevice device;
    TileCompositeBakePoc poc;
    ASSERT_TRUE(poc.initialize(&device, cfg()));
    ASSERT_TRUE(poc.ensureResources());
    const TileCompositeBakeFrameStats s = poc.tick(0);
    EXPECT_EQ(s.bakedTiles, 0);
    EXPECT_EQ(device.beginPassCount, 0);
    EXPECT_EQ(s.bakeBytes, 0);
}
