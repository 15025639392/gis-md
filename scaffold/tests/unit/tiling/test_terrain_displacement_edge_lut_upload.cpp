// H-B1(2026-08-21):边缘 LUT 上传的内容变更检测。
// updateEdgeLutRows 对相同字节跳过 GPU 上传 —— 108 瓦视野惯性期每帧
// 108 次小纹理上传(frameState 6.8-11.4ms)的根因。用 MockRenderDevice
// 的 updateTextureRegion 计数验证:相同字节只传一次,内容变化才再传。

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

    const int n = kGridSize + 1;
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

    const int uploadsBefore = device.textureRegionUpdateCount;
    EXPECT_TRUE(pool.updateEdgeLutRows(key, kGridSize, lut.data()));
    EXPECT_EQ(device.textureRegionUpdateCount, uploadsBefore + 1);

    // 相同字节 → 跳过 GPU 上传(修复点)。
    EXPECT_TRUE(pool.updateEdgeLutRows(key, kGridSize, lut.data()));
    EXPECT_EQ(device.textureRegionUpdateCount, uploadsBefore + 1)
        << "相同 LUT 字节不应再次上传";

    // 内容变化 → 上传并更新缓存。
    lut[0] ^= 0xFF;
    EXPECT_TRUE(pool.updateEdgeLutRows(key, kGridSize, lut.data()));
    EXPECT_EQ(device.textureRegionUpdateCount, uploadsBefore + 2);
}

}  // namespace
