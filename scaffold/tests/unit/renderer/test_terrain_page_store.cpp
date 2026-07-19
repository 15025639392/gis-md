#include <gtest/gtest.h>

#include <cmath>

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

// ---------------- SVT 间接纹理 RGBA8 层编解码(Step B1)----------------

// 编 layer → RGBA8 → 解码回 layer,逐位镜像片元 shader(R+G*256)。
// 覆盖边界:0(全零)/255(R 满)/256(进 G 位)/511(R 满+G=1)/大值(双通道)。
TEST(TerrainPageStoreIndir, EncodeDecodeRoundTrip) {
    for (int layer : {0, 1, 127, 254, 255, 256, 257, 511, 512,
                      1000, 4095, 4096, 12345, 65535}) {
        uint8_t rgba[4] = {0, 0, 0, 0};
        TerrainPageStore::encodeLayerRGBA8(layer, rgba);
        EXPECT_EQ(TerrainPageStore::decodeLayerRGBA8(rgba), layer)
            << "layer=" << layer;
    }
}

// 编码约定:R=layer&0xFF、G=(layer>>8)&0xFF、B=0、A=255。
TEST(TerrainPageStoreIndir, EncodeChannelLayout) {
    uint8_t rgba[4] = {9, 9, 9, 9};
    TerrainPageStore::encodeLayerRGBA8(0, rgba);
    EXPECT_EQ(rgba[0], 0);
    EXPECT_EQ(rgba[1], 0);
    EXPECT_EQ(rgba[2], 0);    // B 保留 0
    EXPECT_EQ(rgba[3], 255);  // A 保留 255(不透明)

    TerrainPageStore::encodeLayerRGBA8(255, rgba);
    EXPECT_EQ(rgba[0], 255);  // R 满
    EXPECT_EQ(rgba[1], 0);

    TerrainPageStore::encodeLayerRGBA8(256, rgba);
    EXPECT_EQ(rgba[0], 0);    // R 归零
    EXPECT_EQ(rgba[1], 1);    // 进 G 位

    TerrainPageStore::encodeLayerRGBA8(513, rgba);
    EXPECT_EQ(rgba[0], 1);    // 513 = 1 + 2*256
    EXPECT_EQ(rgba[1], 2);
}

// 解码逐位镜像片元 shader:floor(r*255+0.5)+floor(g*255+0.5)*256。
// 直接用「采样后的 unorm 值」路径(byte/255→*255→floor+0.5)验与整数解码一致。
TEST(TerrainPageStoreIndir, DecodeMirrorsShaderMath) {
    for (int R = 0; R <= 255; R += 17) {
        for (int G = 0; G <= 255; G += 51) {
            const uint8_t rgba[4] = {static_cast<uint8_t>(R),
                                     static_cast<uint8_t>(G), 0, 255};
            const int viaHelper = TerrainPageStore::decodeLayerRGBA8(rgba);
            // shader 路径:unorm 采样 r=R/255,floor(r*255+0.5)=R。
            const float rf = static_cast<float>(R) / 255.0f;
            const float gf = static_cast<float>(G) / 255.0f;
            const int viaShader =
                static_cast<int>(std::floor(rf * 255.0f + 0.5f)) +
                static_cast<int>(std::floor(gf * 255.0f + 0.5f)) * 256;
            EXPECT_EQ(viaHelper, viaShader) << "R=" << R << " G=" << G;
            EXPECT_EQ(viaHelper, R + G * 256);
        }
    }
}

TEST(TerrainPageStore, TickBeforeAnyTileIsNoop) {
    MockRenderDevice device;
    TerrainPageStore store;
    ASSERT_TRUE(store.initialize(&device, TerrainPageStore::Config{}));
    store.tick();  // 无 entry、无 provider → 不崩、无上传
    EXPECT_EQ(store.uploadedLayerTotal(), 0);
}
