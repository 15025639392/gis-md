#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/TerrainDisplacementTemplatePool.h"
#include "earth_engine/tiling/TileScheme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace earth_engine;

// =============================================================================
// T-P2 量化:非 GLES 后端回退 CPU 烘焙高度层。
//
// GPU 烘焙只有 GLSL(仅 OpenGLES 可用);Metal/Vulkan 恒回退 CPU 烘焙
// (TerrainDisplacementTemplatePool.cpp:401 后端守卫)。债面:CPU 烘焙相对
// GPU 烘焙的成本差没测过。
//
// host 可测的:CPU 烘焙纯函数 `bakeTerrainHeightNormalTexels` 每瓦成本
// (coarse 65² / dense 257² 两档),以及 GPU 路径 CPU 侧(srcPacked 打包)的
// 开销差。GPU RTT pass 本体需真机/离屏 GL(见 T-P6),host 不冒充。
// =============================================================================

namespace {

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// 生产 514 结构(cell-registered + 1px 重叠环)的解析高度场,确定性。
DecodedHeightmap makeHeightmap(const Rectangle& bounds) {
    constexpr int kTileSize = 514;
    constexpr float kInset = 0.5f;
    const double span = static_cast<double>(kTileSize - 1) - 2.0 * kInset;
    DecodedHeightmap hm;
    hm.tileSize = kTileSize;
    hm.borderInset = kInset;
    hm.stagedHeights.resize(static_cast<size_t>(kTileSize) * kTileSize);

    float minH = 1e30f;
    float maxH = -1e30f;
    for (int py = 0; py < kTileSize; ++py) {
        const double v = (static_cast<double>(py) - kInset) / span;
        const double lat = bounds.north() - v * bounds.height();
        for (int px = 0; px < kTileSize; ++px) {
            const double u = (static_cast<double>(px) - kInset) / span;
            const double lon = bounds.west() + u * bounds.width();
            const float h = static_cast<float>(
                1200.0 * std::sin(1.0 * lon) * std::cos(1.0 * lat) +
                400.0 * std::sin(3.0 * lon) +
                300.0 * std::cos(2.0 * lat));
            hm.stagedHeights[static_cast<size_t>(py) * kTileSize + px] = h;
            minH = std::min(minH, h);
            maxH = std::max(maxH, h);
        }
    }
    hm.assignHeights();
    hm.minHeight = minH;
    hm.maxHeight = maxH;
    return hm;
}

double measureBake(const DecodedHeightmap& hm,
                   const Rectangle& bounds,
                   int gridSize,
                   int warmup,
                   int samples,
                   std::vector<uint8_t>& outBytes) {
    const float minH = hm.minHeight;
    const float range =
        std::max(1e-3f, hm.maxHeight - hm.minHeight);
    for (int i = 0; i < warmup; ++i) {
        outBytes = bakeTerrainHeightNormalTexels(
            hm, bounds, gridSize, minH, range);
    }
    std::vector<double> samplesMs;
    samplesMs.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        const Clock::time_point start = Clock::now();
        outBytes = bakeTerrainHeightNormalTexels(
            hm, bounds, gridSize, minH, range);
        samplesMs.push_back(elapsedMs(start, Clock::now()));
    }
    std::sort(samplesMs.begin(), samplesMs.end());
    return std::accumulate(samplesMs.begin(), samplesMs.end(), 0.0) /
           static_cast<double>(samplesMs.size());
}

// GPU 路径的 CPU 侧工作:把 quantizedHeights 打包成 RGBA8 srcPacked
// (TerrainDisplacementTemplatePool.cpp:426-432 同式),作为路径成本差参照。
double measureGpuPathCpuPrep(const DecodedHeightmap& hm,
                             int warmup,
                             int samples,
                             std::vector<uint8_t>& outBytes) {
    const size_t px = static_cast<size_t>(hm.tileSize) * hm.tileSize;
    for (int i = 0; i < warmup; ++i) {
        outBytes.assign(px * 4, 0);
        for (size_t p = 0; p < px; ++p) {
            const uint16_t code = hm.quantizedHeights[p];
            outBytes[p * 4 + 0] = static_cast<uint8_t>(code >> 8);
            outBytes[p * 4 + 1] = static_cast<uint8_t>(code & 0xFF);
        }
    }
    std::vector<double> samplesMs;
    samplesMs.reserve(samples);
    for (int i = 0; i < samples; ++i) {
        const Clock::time_point start = Clock::now();
        outBytes.assign(px * 4, 0);
        for (size_t p = 0; p < px; ++p) {
            const uint16_t code = hm.quantizedHeights[p];
            outBytes[p * 4 + 0] = static_cast<uint8_t>(code >> 8);
            outBytes[p * 4 + 1] = static_cast<uint8_t>(code & 0xFF);
        }
        samplesMs.push_back(elapsedMs(start, Clock::now()));
    }
    std::sort(samplesMs.begin(), samplesMs.end());
    return std::accumulate(samplesMs.begin(), samplesMs.end(), 0.0) /
           static_cast<double>(samplesMs.size());
}

}  // namespace

TEST(TerrainCpuBakeCost, CoarseAndDensePerTile) {
    // 重庆 z12 瓦片的真实经纬包围盒(生产 Terrain-RGB 514 源的同级形状)。
    std::unique_ptr<TileScheme> schemeHolder =
        TileScheme::createXYZWebMercator();
    const TileScheme* scheme = schemeHolder.get();
    const Rectangle bounds = scheme->tileToRectangle(
        TileKey{"XYZ-WebMercator", 12, 3400, 1500});
    const DecodedHeightmap hm = makeHeightmap(bounds);
    ASSERT_TRUE(hm.valid());

    const int grids[] = {64, 256};
    double coarseMs = 0.0;
    double denseMs = 0.0;
    std::vector<uint8_t> bytes;
    for (int grid : grids) {
        const double ms =
            measureBake(hm, bounds, grid, 20, 200, bytes);
        const int n = grid + 1;
        EXPECT_EQ(static_cast<size_t>(n) * n * 4, bytes.size());
        if (grid == 64) {
            coarseMs = ms;
        } else {
            denseMs = ms;
        }
        RecordProperty(("cpu_bake_grid_" + std::to_string(grid) + "_ms")
                           .c_str(),
                       ms);
        std::printf("[T-P2] CPU 烘焙 grid=%d:%.5f ms/瓦(%d² texels)\n",
                    grid, ms, n);
    }
    std::printf("[T-P2] 20 瓦 dense 一帧 = %.2f ms;coarse = %.2f ms\n",
                denseMs * 20.0, coarseMs * 20.0);
    // 方向性 sanity:dense(257²) texel 数是 coarse(65²) 的 ~15.6×。
    EXPECT_GT(denseMs, coarseMs * 2.0)
        << "dense 烘焙未显著慢于 coarse,测量可疑";

    // GPU 路径 CPU 侧打包参照(与 CPU 烘焙同源输入)。
    std::vector<uint8_t> packed;
    const double prepMs =
        measureGpuPathCpuPrep(hm, 20, 200, packed);
    RecordProperty("gpu_path_cpu_prep_ms", prepMs);
    std::printf(
        "[T-P2] GPU 路径 CPU 侧打包(514² 源)=%.5f ms/瓦 —— 与 CPU 烘焙差值 %.5f ms\n",
        prepMs, denseMs - prepMs);
}
