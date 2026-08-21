#include <gtest/gtest.h>

#include "earth_engine/content/EllipsoidTerrainMeshBuilder.h"
#include "earth_engine/content/GltfModel.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/WebMercatorProjection.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/math/Vec3.h"
#include "earth_engine/tiling/RasterOverlayProjection.h"
#include "earth_engine/tiling/TileFillProxyPreparer.h"
#include "earth_engine/tiling/TilesetTile.h"
#include "../../helpers/MockRenderDevice.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <utility>
#include <vector>

using namespace earth_engine;

// =============================================================================
// T-P3 量化:fill 代理构建同步且无帧预算封顶
// (TileUpdateSelectionWorkRunner.h:237 对每个可见瓦片循环 ensureFillProxy,
//  无 break/budget,fillStartMs 仅事后计时)。
//
// 每瓦全量构建 = 签名(廉价)+ 网格(17² 顶点+texcoord+裙墙)+ 高度采样
// (有祖先高度源时 17² 次双线性)+ buildTerrainVertices 打包 + 2 次 createBuffer。
// 本测试走生产 ensureFillProxy(共享 MockRenderDevice),测:
//   - 每瓦全量构建 ms(Debug 口径,附 Release 对照)
//   - 首见帧 burst(K 瓦一帧)总 ms
//   - gridSize 敏感性(网格顶点数 ~grid²)
//   - 祖先高度采样循环的增量成本(17² 次双线性 vs 无采样)
// 注意:Debug -O0 会把同路径放大 ~8×(T-P7 实测 129ms vs release 16ms),
// 真机对标必须 Release 口径。
// =============================================================================

namespace {

using Clock = std::chrono::steady_clock;

double elapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// 真实 WebMercator 瓦片的经纬包围盒(OpenGlobus-Earth 地形 scheme 同源)。
Rectangle webMercatorTileBounds(int zoom, int x, int y) {
    const WebMercatorProjection proj(Ellipsoid::WGS84());
    const double tiles = static_cast<double>(1u << zoom);
    const double size = MathUtils::TwoPi / tiles;
    const double westMerc = static_cast<double>(x) * size - MathUtils::OnePi;
    const double eastMerc =
        static_cast<double>(x + 1) * size - MathUtils::OnePi;
    const double northMerc = MathUtils::OnePi -
                             static_cast<double>(y) * size;
    const double southMerc = MathUtils::OnePi -
                             static_cast<double>(y + 1) * size;
    const double r = proj.semimajorAxis();
    const Cartographic nw =
        proj.unproject(Vec3(westMerc * r, northMerc * r, 0.0));
    const Cartographic se =
        proj.unproject(Vec3(eastMerc * r, southMerc * r, 0.0));
    return Rectangle(nw.longitude(), se.latitude(),
                     se.longitude(), nw.latitude());
}

std::pair<int, int> webMercatorTileXy(double lngRad, double latRad, int zoom) {
    const double tiles = static_cast<double>(1u << zoom);
    const int x = static_cast<int>(std::floor(
        (lngRad + MathUtils::OnePi) / MathUtils::TwoPi * tiles));
    const double sinLat = std::sin(latRad);
    const double mercY = 0.5 * std::log(
        (1.0 + sinLat) / (1.0 - sinLat));
    const int y = static_cast<int>(std::floor(
        (1.0 - mercY / MathUtils::OnePi) * 0.5 * tiles));
    return {x, y};
}

// 重庆 z12 一带的真实瓦片(默认网格 16 的构建成本基准)。
Rectangle benchmarkBounds() {
    const auto [x, y] = webMercatorTileXy(
        106.508 * MathUtils::OnePi / 180.0,
        29.617 * MathUtils::OnePi / 180.0,
        12);
    return webMercatorTileBounds(12, x, y);
}

// 走生产 ensureFillProxy 全量构建一个新鲜瓦片,返回是否 madeReady。
bool fullBuild(TilesetTile& tile,
               earth_engine::testing::MockRenderDevice& device,
               int gridSize) {
    const TileFillProxyPrepareResult result =
        TileFillProxyPreparer::ensureFillProxy(
            tile, &device, gridSize);
    return result.madeReady;
}

}  // namespace

// ---- 每瓦全量构建成本 + 稳态签名早退 ----
TEST(FillProxyBuildCost, PerTileFullBuildAndSteadyStateHit) {
    const int gridSize = 16;  // 配置默认 terrainFillProxyGridSize
    const int kBuilds = 100;
    const int kWarmup = 10;
    const Rectangle bounds = benchmarkBounds();

    for (int i = 0; i < kWarmup; ++i) {
        TilesetTile tile(
            TileKey{"OpenGlobus-Earth", 12, 0, i % 2},
            bounds);
        earth_engine::testing::MockRenderDevice device;
        ASSERT_TRUE(fullBuild(tile, device, gridSize));
    }

    std::vector<double> perBuildMs;
    perBuildMs.reserve(kBuilds);
    for (int i = 0; i < kBuilds; ++i) {
        TilesetTile tile(
            TileKey{"OpenGlobus-Earth", 12, 0, i % 4},
            bounds);
        earth_engine::testing::MockRenderDevice device;
        const Clock::time_point start = Clock::now();
        ASSERT_TRUE(fullBuild(tile, device, gridSize));
        perBuildMs.push_back(elapsedMs(start, Clock::now()));
        ASSERT_EQ(2, device.createdBufferCount) << "全量构建应上传 2 个 buffer";
    }
    std::sort(perBuildMs.begin(), perBuildMs.end());
    const double mean = std::accumulate(
                            perBuildMs.begin(), perBuildMs.end(), 0.0) /
                        static_cast<double>(perBuildMs.size());
    const double median = perBuildMs[perBuildMs.size() / 2];
    const double min = perBuildMs.front();
    RecordProperty("per_tile_build_mean_ms", mean);
    RecordProperty("per_tile_build_median_ms", median);
    RecordProperty("per_tile_build_min_ms", min);
    std::printf("[T-P3] 每瓦全量构建 mean=%.4f ms median=%.4f min=%.4f (n=%d, grid=%d)\n",
                mean, median, min, kBuilds, gridSize);

    // 稳态签名早退:同一瓦再次 ensureFillProxy 应为 no-op(零 buffer)。
    TilesetTile steady(
        TileKey{"OpenGlobus-Earth", 12, 0, 99},
        bounds);
    earth_engine::testing::MockRenderDevice device;
    ASSERT_TRUE(fullBuild(steady, device, gridSize));
    const int buffersAfterBuild = device.createdBufferCount;
    const int kHits = 1000;
    const Clock::time_point hitStart = Clock::now();
    for (int i = 0; i < kHits; ++i) {
        EXPECT_FALSE(TileFillProxyPreparer::ensureFillProxy(
            steady, &device, gridSize));
    }
    const double hitMs = elapsedMs(hitStart, Clock::now()) /
                         static_cast<double>(kHits);
    EXPECT_EQ(buffersAfterBuild, device.createdBufferCount)
        << "稳态签名命中不应再上传 buffer";
    RecordProperty("steady_state_hit_ms", hitMs);
    std::printf("[T-P3] 稳态签名命中 mean=%.5f ms (n=%d, 0 buffer)\n",
                hitMs, kHits);
}

// ---- 首见帧 burst:一次飞行/大跳变 K 瓦一帧全量构建 ----
TEST(FillProxyBuildCost, BurstScenarioCost) {
    const int gridSize = 16;
    const Rectangle bounds = benchmarkBounds();
    const int bursts[] = {32, 64, 128};
    for (int k : bursts) {
        earth_engine::testing::MockRenderDevice device;
        const Clock::time_point start = Clock::now();
        int built = 0;
        for (int i = 0; i < k; ++i) {
            TilesetTile tile(
                TileKey{"OpenGlobus-Earth", 12, i % 8, i % 8},
                bounds);
            if (fullBuild(tile, device, gridSize)) {
                ++built;
            }
        }
        const double totalMs = elapsedMs(start, Clock::now());
        RecordProperty(("burst_" + std::to_string(k) + "_ms").c_str(),
                       totalMs);
        RecordProperty(("burst_" + std::to_string(k) + "_per_tile_ms").c_str(),
                       totalMs / static_cast<double>(k));
        std::printf("[T-P3] burst %d 瓦一帧 = %.3f ms(%.4f ms/瓦,built=%d)\n",
                    k, totalMs, totalMs / static_cast<double>(k), built);
        EXPECT_EQ(k, built);
    }
}

// ---- gridSize 敏感性:网格顶点数 ~grid²,成本应随 grid 显著增长 ----
TEST(FillProxyBuildCost, GridSizeSensitivity) {
    const Rectangle bounds = benchmarkBounds();
    const int kBuilds = 50;
    const int grids[] = {8, 16, 32};
    std::vector<double> means;
    for (int grid : grids) {
        earth_engine::testing::MockRenderDevice device;
        std::vector<double> samples;
        samples.reserve(kBuilds);
        for (int i = 0; i < kBuilds; ++i) {
            TilesetTile tile(
                TileKey{"OpenGlobus-Earth", 12, 0, i % 4},
                bounds);
            const Clock::time_point start = Clock::now();
            ASSERT_TRUE(fullBuild(tile, device, grid));
            samples.push_back(elapsedMs(start, Clock::now()));
        }
        std::sort(samples.begin(), samples.end());
        const double mean = std::accumulate(
                                samples.begin(), samples.end(), 0.0) /
                            static_cast<double>(samples.size());
        means.push_back(mean);
        RecordProperty(("grid_" + std::to_string(grid) + "_mean_ms").c_str(),
                       mean);
        std::printf("[T-P3] grid=%2d 每瓦 mean=%.4f ms (n=%d)\n",
                    grid, mean, kBuilds);
    }
    // 方向性 sanity:grid 32 至少显著大于 grid 8(网格工作量 16×;噪声下
    // 用 0.5× 保守界只钉方向,不钉精确倍数)。
    EXPECT_GT(means[2], means[0] * 0.5)
        << "成本未随 gridSize 增长,疑似固定开销主导(H2)";
}

// ---- 祖先高度采样循环增量成本(17² 次采样 vs 无采样) ----
TEST(FillProxyBuildCost, HeightSourceSamplingLoopCost) {
    const Rectangle bounds = benchmarkBounds();
    const int gridSize = 16;
    const int kBuilds = 100;

    double noSampleMs = 0.0;
    double withSampleMs = 0.0;
    volatile double sink = 0.0;
    for (int i = 0; i < kBuilds; ++i) {
        const Clock::time_point a = Clock::now();
        auto flat = EllipsoidTerrainMeshBuilder::makeModel(
            bounds,
            std::vector<RasterOverlayProjection>{
                RasterOverlayProjection::WebMercator},
            gridSize,
            {},
            /*computeGridNormals=*/false,
            /*computeGeomorphDelta=*/false,
            /*buildSkirt=*/true);
        noSampleMs += elapsedMs(a, Clock::now());
        ASSERT_NE(nullptr, flat);

        EllipsoidProxyHeightSampler sampler =
            [](double lng, double lat) -> std::optional<float> {
            return std::optional<float>(
                static_cast<float>(std::sin(lng * 37.0 + lat * 11.0) * 250.0));
        };
        const Clock::time_point b = Clock::now();
        auto lifted = EllipsoidTerrainMeshBuilder::makeModel(
            bounds,
            std::vector<RasterOverlayProjection>{
                RasterOverlayProjection::WebMercator},
            gridSize,
            sampler,
            /*computeGridNormals=*/true,
            /*computeGeomorphDelta=*/false,
            /*buildSkirt=*/true);
        withSampleMs += elapsedMs(b, Clock::now());
        ASSERT_NE(nullptr, lifted);
        sink += lifted->primitives.front().vertices[0].normalEcef.x();
    }
    (void)sink;
    const double perTileDelta = (withSampleMs - noSampleMs) /
                                static_cast<double>(kBuilds);
    RecordProperty("height_sample_delta_per_tile_ms", perTileDelta);
    RecordProperty("no_sample_mean_ms", noSampleMs / kBuilds);
    RecordProperty("with_sample_mean_ms", withSampleMs / kBuilds);
    std::printf(
        "[T-P3] 高度采样增量 mean=%.4f ms/瓦(无采样 %.4f → 有采样 %.4f, n=%d)\n",
        perTileDelta, noSampleMs / kBuilds, withSampleMs / kBuilds, kBuilds);
}
