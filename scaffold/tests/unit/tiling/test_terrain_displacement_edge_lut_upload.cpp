// H-B1 + H-S4(2026-08-21):边缘 LUT 上传的内容变更检测 + 批量上传。
// H-B1:updateEdgeLutRows 对相同字节跳过上传 —— 108 瓦视野惯性期每帧
// 108 次小纹理上传(frameState 6.8-11.4ms)的根因。
// H-S4:上传从「逐层立即调用」改为「入池 + 帧末 flushEdgeLutUploads 一次
// 批量上传」;acquire 的 delta-0 初始化也并入批。用 MockRenderDevice 的
// textureArrayRegionUpdateCount 验证:同帧多层变更合并成一次批量调用。

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TerrainDisplacementTemplatePool.h"
#include "earth_engine/tiling/TileKey.h"
#include "../../helpers/MockRenderDevice.h"

using namespace earth_engine;
using earth_engine::testing::MockRenderDevice;

namespace {

DecodedHeightmap makeHeightmap(int tileSize) {
    DecodedHeightmap hm;
    hm.tileSize = tileSize;
    hm.quantizedHeights.assign(
        static_cast<size_t>(tileSize) * static_cast<size_t>(tileSize), 32768);
    hm.quantBase = 0;
    hm.minHeight = 0.0f;
    hm.maxHeight = 100.0f;
    return hm;
}

std::vector<uint8_t> makeZeroDeltaLut(int gridSize) {
    const int n = gridSize + 1;
    std::vector<uint8_t> lut(
        static_cast<size_t>(n) *
            static_cast<size_t>(
                TerrainDisplacementTemplatePool::kEdgeLutRows) *
            4u,
        0);
    const uint16_t q =
        TerrainDisplacementTemplatePool::encodeEdgeLutDelta(0.0f);
    for (size_t i = 0; i < lut.size(); i += 4) {
        lut[i] = static_cast<uint8_t>(q >> 8);
        lut[i + 1] = static_cast<uint8_t>(q & 0xFF);
    }
    return lut;
}

TEST(TerrainDisplacementEdgeLutUpload, IdenticalBytesSkipUpload) {
    MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;  // 允许高度层/LUT 上传
    TerrainDisplacementTemplatePool pool;
    pool.initialize(&device);
    pool.setGpuHeightBakeEnabled(false);  // 走 CPU 烘焙,避免 mock 无 GL 上下文

    const TileKey key{SchemeId{}, 8, 130, 90};
    const Rectangle bounds(0.0, 0.0, 0.1, 0.1);
    constexpr int kGridSize = 64;
    const DecodedHeightmap hm = makeHeightmap(65);
    const auto* ht =
        pool.acquireHeightTexture(key, hm, bounds, kGridSize, 1);
    ASSERT_NE(ht, nullptr) << "高度层获取失败:测试台没搭起来";

    std::vector<uint8_t> lut = makeZeroDeltaLut(kGridSize);

    // acquire 只把 delta-0 初始化入池,尚未发生任何上传。
    EXPECT_EQ(device.textureArrayRegionUpdateCount, 0)
        << "H-S4:acquire 的 LUT 初始化应入批,不立即上传";

    // 真实差值替换 init,帧末一次批量上传(单层)。
    EXPECT_TRUE(pool.updateEdgeLutRows(key, kGridSize, lut.data()));
    EXPECT_TRUE(pool.flushEdgeLutUploads());
    EXPECT_EQ(device.textureArrayRegionUpdateCount, 1)
        << "H-S4:同帧同 array 的多层变更应合并成一次批量调用";
    EXPECT_EQ(device.lastBatchFirstLayer, 0);
    EXPECT_EQ(device.lastBatchLayerCount, 1);

    // 相同字节 → 跳过(字节 diff 在入池前,缓存已随上次 flush 推进)。
    EXPECT_TRUE(pool.updateEdgeLutRows(key, kGridSize, lut.data()));
    EXPECT_TRUE(pool.flushEdgeLutUploads());
    EXPECT_EQ(device.textureArrayRegionUpdateCount, 1)
        << "相同 LUT 字节不应再次上传";

    // 内容变化 → 上传并更新缓存。
    lut[0] ^= 0xFF;
    EXPECT_TRUE(pool.updateEdgeLutRows(key, kGridSize, lut.data()));
    EXPECT_TRUE(pool.flushEdgeLutUploads());
    EXPECT_EQ(device.textureArrayRegionUpdateCount, 2);
}

TEST(TerrainDisplacementEdgeLutUpload, ChangedLayersBatchIntoSingleCall) {
    MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    TerrainDisplacementTemplatePool pool;
    pool.initialize(&device);
    pool.setGpuHeightBakeEnabled(false);

    const Rectangle bounds(0.0, 0.0, 0.1, 0.1);
    constexpr int kGridSize = 64;
    const DecodedHeightmap hm = makeHeightmap(65);
    const TileKey keyA{SchemeId{}, 8, 130, 90};
    const TileKey keyB{SchemeId{}, 8, 131, 90};
    ASSERT_NE(pool.acquireHeightTexture(keyA, hm, bounds, kGridSize, 1),
              nullptr);
    ASSERT_NE(pool.acquireHeightTexture(keyB, hm, bounds, kGridSize, 1),
              nullptr);

    std::vector<uint8_t> lutA = makeZeroDeltaLut(kGridSize);
    std::vector<uint8_t> lutB = makeZeroDeltaLut(kGridSize);
    lutA[0] = 0xAB;  // 两个瓦片的差值各不相同,都必须上传
    lutB[0] = 0xCD;
    EXPECT_TRUE(pool.updateEdgeLutRows(keyA, kGridSize, lutA.data()));
    EXPECT_TRUE(pool.updateEdgeLutRows(keyB, kGridSize, lutB.data()));

    EXPECT_TRUE(pool.flushEdgeLutUploads());
    EXPECT_EQ(device.textureArrayRegionUpdateCount, 1)
        << "H-S4:两层变更应合并成一次批量上传";
    EXPECT_EQ(device.lastBatchFirstLayer, 0);
    EXPECT_EQ(device.lastBatchLayerCount, 2);
}

TEST(TerrainDisplacementEdgeLutUpload, FlushFailureIsReported) {
    MockRenderDevice device;
    device.textureRegionUploadSucceeds = true;
    TerrainDisplacementTemplatePool pool;
    pool.initialize(&device);
    pool.setGpuHeightBakeEnabled(false);

    constexpr int kGridSize = 64;
    const TileKey key{SchemeId{}, 8, 130, 90};
    const Rectangle bounds(0.0, 0.0, 0.1, 0.1);
    const DecodedHeightmap hm = makeHeightmap(65);
    ASSERT_NE(pool.acquireHeightTexture(key, hm, bounds, kGridSize, 1),
              nullptr);
    std::vector<uint8_t> lut = makeZeroDeltaLut(kGridSize);
    lut[0] ^= 0x7f;
    ASSERT_TRUE(pool.updateEdgeLutRows(key, kGridSize, lut.data()));
    device.textureRegionUploadSucceeds = false;
    EXPECT_FALSE(pool.flushEdgeLutUploads());
}

}  // namespace
