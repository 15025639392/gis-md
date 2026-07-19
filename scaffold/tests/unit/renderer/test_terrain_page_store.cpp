#include <gtest/gtest.h>

#include "earth_engine/renderer/TerrainPageStore.h"
#include "../../helpers/MockRenderDevice.h"

using namespace earth_engine;
using earth_engine::testing::MockRenderDevice;

namespace {

// blockLayers=16(gridN=4), 3 块 → totalLayers=48。
TerrainPageLayerPool makePool(int blockCount = 3, int blockLayers = 16) {
    TerrainPageLayerPool pool;
    pool.configure(blockCount, blockLayers);
    return pool;
}

}  // namespace

// ---------------- TerrainPageLayerPool(纯 CPU 分配器)----------------

TEST(TerrainPageLayerPool, AcquireGivesContiguousDistinctBlocks) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 123;
    EXPECT_EQ(pool.acquire(/*key=*/10, /*frame=*/1, &ev), 0);
    EXPECT_EQ(ev, 0u);
    EXPECT_EQ(pool.acquire(11, 1, &ev), 16);
    EXPECT_EQ(pool.acquire(12, 1, &ev), 32);
    EXPECT_EQ(pool.residentCount(), 3);
    // 各块 layerBase 连续、块尺寸 16。
    EXPECT_EQ(pool.blockLayers(), 16);
    EXPECT_EQ(pool.blockCount(), 3);
}

TEST(TerrainPageLayerPool, ReacquireResidentReturnsSameBaseNoEvict) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 0;
    const int base = pool.acquire(10, 1, &ev);
    // 已驻留:同 key 再取返回同 base、不淘汰。
    EXPECT_EQ(pool.acquire(10, 2, &ev), base);
    EXPECT_EQ(ev, 0u);
    EXPECT_EQ(pool.residentCount(), 1);
    EXPECT_EQ(pool.layerBaseFor(10), base);
    EXPECT_EQ(pool.layerBaseFor(999), -1);  // 未驻留
}

TEST(TerrainPageLayerPool, EvictsLeastRecentlyUsedWhenFull) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 0;
    pool.acquire(10, /*frame=*/1, &ev);  // slot0
    pool.acquire(11, /*frame=*/2, &ev);  // slot1
    pool.acquire(12, /*frame=*/3, &ev);  // slot2
    // 第 4 个(frame=4):池满 → 淘汰 lastFrame 最小者(key 10,frame 1)。
    const int base = pool.acquire(13, 4, &ev);
    EXPECT_EQ(ev, 10u);
    EXPECT_EQ(base, 0);  // 复用 slot0 的 base
    EXPECT_EQ(pool.layerBaseFor(10), -1);  // 被淘汰
    EXPECT_EQ(pool.layerBaseFor(13), 0);
    EXPECT_EQ(pool.residentCount(), 3);
}

TEST(TerrainPageLayerPool, TouchUpdatesRecencyProtectsFromEviction) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 0;
    pool.acquire(10, 1, &ev);
    pool.acquire(11, 2, &ev);
    pool.acquire(12, 3, &ev);
    // frame 4:touch key 10(它本是最久),使 11 变最久。
    pool.acquire(10, 4, &ev);
    EXPECT_EQ(ev, 0u);
    // frame 5:池满 → 现最久是 key 11(frame 2)。
    pool.acquire(13, 5, &ev);
    EXPECT_EQ(ev, 11u);
    EXPECT_EQ(pool.layerBaseFor(10), 0);   // 因 touch 存活
    EXPECT_EQ(pool.layerBaseFor(11), -1);  // 被淘汰
}

TEST(TerrainPageLayerPool, RefusesEvictionWhenAllTouchedThisFrame) {
    TerrainPageLayerPool pool = makePool(2, 16);
    uint64_t ev = 0;
    pool.acquire(10, /*frame=*/7, &ev);  // 本帧
    pool.acquire(11, /*frame=*/7, &ev);  // 本帧
    // 第三个瓦片同帧:两块都是本帧可见 → 不淘汰,返回 -1(调用方回落 mappedRaster)。
    const int base = pool.acquire(12, 7, &ev);
    EXPECT_EQ(base, -1);
    EXPECT_EQ(ev, 0u);
    EXPECT_EQ(pool.residentCount(), 2);
}

TEST(TerrainPageLayerPool, ReleaseFreesBlockForReuse) {
    TerrainPageLayerPool pool = makePool(2, 16);
    uint64_t ev = 0;
    const int b0 = pool.acquire(10, 1, &ev);
    pool.acquire(11, 1, &ev);
    pool.release(10);
    EXPECT_EQ(pool.layerBaseFor(10), -1);
    EXPECT_EQ(pool.residentCount(), 1);
    // 释放后空块可再分配(同帧也行,因不需淘汰)。
    const int reused = pool.acquire(12, 1, &ev);
    EXPECT_EQ(reused, b0);
    EXPECT_EQ(ev, 0u);
    pool.release(999);  // 不存在 → no-op
}

TEST(TerrainPageLayerPool, ConfigureResetsResidency) {
    TerrainPageLayerPool pool = makePool(3, 16);
    uint64_t ev = 0;
    pool.acquire(10, 1, &ev);
    pool.configure(2, 4);  // 重配 → 清空
    EXPECT_EQ(pool.residentCount(), 0);
    EXPECT_EQ(pool.blockCount(), 2);
    EXPECT_EQ(pool.blockLayers(), 4);
    EXPECT_EQ(pool.layerBaseFor(10), -1);
}

// ---------------- TerrainPageStore(创建/门控)----------------

TEST(TerrainPageStore, InitFailsOnNullDevice) {
    TerrainPageStore store;
    EXPECT_FALSE(store.initialize(nullptr, TerrainPageStore::Config{}));
    EXPECT_FALSE(store.isReady());
}

TEST(TerrainPageStore, InitializeCreatesSharedArrayTexture) {
    MockRenderDevice device;
    TerrainPageStore store;
    TerrainPageStore::Config cfg;
    cfg.depthLevels = 2;       // gridN=4 → 16 层/块
    cfg.maxResidentTiles = 4;  // 4 块 → 64 层
    ASSERT_TRUE(store.initialize(&device, cfg));
    EXPECT_TRUE(store.isReady());
    EXPECT_EQ(device.createdTextureCount, 1);
    EXPECT_EQ(device.lastTextureDesc.arrayLayers, 64);  // 16*4
    EXPECT_EQ(device.lastTextureDesc.width, 256);
    EXPECT_EQ(store.residentTileCount(), 0);
    EXPECT_EQ(store.uploadedLayerTotal(), 0);
}

TEST(TerrainPageStore, TickBeforeAnyTileIsNoop) {
    MockRenderDevice device;
    TerrainPageStore store;
    ASSERT_TRUE(store.initialize(&device, TerrainPageStore::Config{}));
    store.tick();  // 无 entry、无 provider → 不崩、无上传
    EXPECT_EQ(store.uploadedLayerTotal(), 0);
}
