#include <gtest/gtest.h>

#include "earth_engine/providers/TerrainProvider.h"
#include "earth_engine/tiling/DecodedHeightmapSampler.h"
#include "earth_engine/tiling/TerrainDisplacementTemplatePool.h"
#include "earth_engine/tiling/TileScheme.h"

using namespace earth_engine;

namespace {

// A geographic rectangle to serve as sourceBounds; any rectangle works since
// the sampler maps lon/lat to [0,1] tile coordinates relative to it.
Rectangle rootBounds() {
    auto scheme = TileScheme::createGeographicTMS();
    return scheme->tileToRectangle(TileKey{"Geographic-TMS", 0, 0, 0});
}

} // namespace

TEST(DecodedHeightmapSamplerTest, SamplesUniformHeight) {
    DecodedHeightmap heightmap;
    heightmap.tileSize = 2;
    heightmap.heights = {42.0f, 42.0f, 42.0f, 42.0f};
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * 0.5;
    const double lat = bounds.south() + bounds.height() * 0.5;
    EXPECT_NEAR(42.0f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}

TEST(DecodedHeightmapSamplerTest, BilinearInterpolatesGradient) {
    // Row-major, north→south rows / west→east columns:
    //   row0 (north): NW=30, NE=40
    //   row1 (south): SW=10, SE=20
    // At u=0.25 (from west), 0.25 from south (→ v=0.75 north→south), the
    // bilinear blend is 17.5.
    DecodedHeightmap heightmap;
    heightmap.tileSize = 2;
    heightmap.heights = {30.0f, 40.0f, 10.0f, 20.0f};
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * 0.25;
    const double lat = bounds.south() + bounds.height() * 0.25;
    EXPECT_NEAR(17.5f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}

TEST(DecodedHeightmapSamplerTest, ReturnsZeroOutsideBounds) {
    DecodedHeightmap heightmap;
    heightmap.tileSize = 2;
    heightmap.heights = {50.0f, 50.0f, 50.0f, 50.0f};
    const Rectangle bounds = rootBounds();

    // A longitude well east of the rectangle's east edge → out of bounds → 0.
    const double lon = bounds.east() + bounds.width();
    const double lat = bounds.south() + bounds.height() * 0.5;
    EXPECT_NEAR(0.0f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}

TEST(DecodedHeightmapSamplerTest, ReturnsZeroForNoData) {
    // All corners are a no-data sentinel (>50000) → sampler reports 0 (sea
    // level) rather than a spurious huge height.
    DecodedHeightmap heightmap;
    heightmap.tileSize = 2;
    heightmap.heights = {65535.0f, 65535.0f, 65535.0f, 65535.0f};
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * 0.5;
    const double lat = bounds.south() + bounds.height() * 0.5;
    EXPECT_NEAR(0.0f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}

TEST(DecodedHeightmapSamplerTest, ReturnsZeroForInvalidHeightmap) {
    DecodedHeightmap heightmap;  // tileSize == 0 → invalid.
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * 0.5;
    const double lat = bounds.south() + bounds.height() * 0.5;
    EXPECT_NEAR(0.0f,
                DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
                1e-4f);
}

// borderInset=0.5 契约:cell-registered + 1px 重叠环源(Mapbox 514)相邻瓦片
// 无缝。用世界线性高程 h(x)=x 构造两片相邻瓦片(tileSize=4 = 2 个内部 cell +
// 每侧 1px 重叠),验证东片西边界采样 == 西片东边界采样(共享边界 x=2.0)。
// 每行相同,退化成 1D 便于推理。
TEST(DecodedHeightmapSamplerTest, BorderInsetHalfPixelSeamless) {
    // 西片覆盖世界 [0,2]:内部 cell 中心 x=0.5,1.5;重叠列 x=-0.5(西邻),2.5(东邻)
    DecodedHeightmap west;
    west.tileSize = 4;
    west.borderInset = 0.5f;
    west.heights.clear();
    for (int r = 0; r < 4; ++r)
        for (float x : {-0.5f, 0.5f, 1.5f, 2.5f}) west.heights.push_back(x);
    // 东片覆盖世界 [2,4]:内部 cell 中心 x=2.5,3.5;重叠列 x=1.5(西邻),4.5(东邻)
    DecodedHeightmap east;
    east.tileSize = 4;
    east.borderInset = 0.5f;
    east.heights.clear();
    for (int r = 0; r < 4; ++r)
        for (float x : {1.5f, 2.5f, 3.5f, 4.5f}) east.heights.push_back(x);

    const float westEast = west.sampleBilinear(1.0f, 0.5f);  // 西片东边界 → x=2.0
    const float eastWest = east.sampleBilinear(0.0f, 0.5f);  // 东片西边界 → x=2.0
    EXPECT_NEAR(2.0f, westEast, 1e-4f);
    EXPECT_NEAR(2.0f, eastWest, 1e-4f);
    EXPECT_NEAR(westEast, eastWest, 1e-5f);  // 无缝:共享边界值完全一致
}

// borderInset=0(默认,顶点栅格源如自产 grid65)保持原映射 [0, tileSize-1]:
// u=0 落在像素 0、u=1 落在最后一像素,不内缩。
TEST(DecodedHeightmapSamplerTest, BorderInsetZeroPreservesVertexGrid) {
    DecodedHeightmap hm;
    hm.tileSize = 3;
    hm.borderInset = 0.0f;
    hm.heights = {0, 10, 20, 0, 10, 20, 0, 10, 20};  // 每行 0,10,20
    EXPECT_NEAR(0.0f, hm.sampleBilinear(0.0f, 0.5f), 1e-4f);   // 像素 0
    EXPECT_NEAR(20.0f, hm.sampleBilinear(1.0f, 0.5f), 1e-4f);  // 像素 2
    EXPECT_NEAR(10.0f, hm.sampleBilinear(0.5f, 0.5f), 1e-4f);  // 像素 1
}

// ============================================================
// 渲染网格一致采样(矢量贴地 P3)
// ============================================================

TEST(DecodedHeightmapSamplerTest, RenderGridIgnoresIntraCellSpike) {
    // 129×129 源 → 渲染格 = min(128,64) = 64 cells,节点落在偶数细格上。
    // 在奇数细格(节点之间)放尖峰:全分辨率采样看得见,渲染网格采样看不见
    // (渲染面就是看不见它 —— 贴地几何必须跟渲染面同源,否则结构性穿插)。
    DecodedHeightmap heightmap;
    heightmap.tileSize = 129;
    heightmap.heights.assign(129 * 129, 0.0f);
    heightmap.heights[static_cast<size_t>(65) * 129 + 65] = 500.0f;  // 奇数格
    const Rectangle bounds = rootBounds();

    // 尖峰所在位置(u = 65/128, v = 65/128 → 北南翻转的纬度)
    const double lon = bounds.west() + bounds.width() * (65.0 / 128.0);
    const double lat = bounds.north() - bounds.height() * (65.0 / 128.0);

    const float fullRes =
        DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat);
    EXPECT_NEAR(500.0f, fullRes, 1.0f);  // 全分辨率:尖峰可见

    const float renderGrid = DecodedHeightmapSampler::sampleHeightRenderGrid(
        heightmap, bounds, lon, lat, kTerrainDisplacementGridSize);
    EXPECT_NEAR(0.0f, renderGrid, 1e-3f);  // 渲染网格:节点全 0 → 面为 0
}

TEST(DecodedHeightmapSamplerTest, RenderGridMatchesFullResAtGridNodes) {
    // 节点上两种采样必须一致(渲染面经过节点)。
    DecodedHeightmap heightmap;
    heightmap.tileSize = 129;
    heightmap.heights.assign(129 * 129, 0.0f);
    heightmap.heights[static_cast<size_t>(64) * 129 + 64] = 300.0f;  // 偶数格=节点
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * (64.0 / 128.0);
    const double lat = bounds.north() - bounds.height() * (64.0 / 128.0);

    const float fullRes =
        DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat);
    const float renderGrid = DecodedHeightmapSampler::sampleHeightRenderGrid(
        heightmap, bounds, lon, lat, kTerrainDisplacementGridSize);
    EXPECT_NEAR(fullRes, renderGrid, 1e-3f);
    EXPECT_NEAR(300.0f, renderGrid, 1.0f);
}

TEST(DecodedHeightmapSamplerTest, RenderGridSmallSourceEqualsFullRes) {
    // 源 ≤ 65:渲染格 = 源格,两种采样恒等。
    DecodedHeightmap heightmap;
    heightmap.tileSize = 5;
    heightmap.heights.assign(25, 0.0f);
    heightmap.heights[12] = 100.0f;  // 中心
    const Rectangle bounds = rootBounds();

    for (double f : {0.1, 0.37, 0.5, 0.73, 0.9}) {
        const double lon = bounds.west() + bounds.width() * f;
        const double lat = bounds.south() + bounds.height() * f;
        EXPECT_NEAR(
            DecodedHeightmapSampler::sampleHeight(heightmap, bounds, lon, lat),
            DecodedHeightmapSampler::sampleHeightRenderGrid(
                heightmap, bounds, lon, lat, kTerrainDisplacementGridSize),
            1e-3f) << "f=" << f;
    }
}

// ============================================================
// 自适应几何密度(解 65×65 钉死)
// ============================================================

TEST(TerrainGridSizeForSseTest, UpgradesOnlyAboveThresholdAndHasHysteresis) {
    // 未定档(0):用升档阈值 64px。
    EXPECT_EQ(kTerrainDisplacementGridSize, terrainGridSizeForSse(63.9, 0));
    EXPECT_EQ(kTerrainDenseGridSize, terrainGridSizeForSse(64.0, 0));

    // 已在 dense 档:用更低的降档阈值 48px = 迟滞带。缺了它,SSE 在阈值附近
    // 抖动的瓦片会逐帧换档,而换档要重建常驻命令 + 重烘 257² 高度纹理。
    EXPECT_EQ(kTerrainDenseGridSize,
              terrainGridSizeForSse(50.0, kTerrainDenseGridSize));
    EXPECT_EQ(kTerrainDisplacementGridSize,
              terrainGridSizeForSse(47.9, kTerrainDenseGridSize));

    // 同一个 SSE(50)在两个档下给出不同结果 —— 这正是迟滞的定义。
    EXPECT_NE(terrainGridSizeForSse(50.0, kTerrainDisplacementGridSize),
              terrainGridSizeForSse(50.0, kTerrainDenseGridSize));
}

TEST(DecodedHeightmapSamplerTest, DenseGridResolvesDetailCoarseGridMisses) {
    // 这条锁死本次改动的**目的**:coarse 档(64 格)在 129 源上每 2 个源像素才
    // 取一个节点,落在奇数源像素上的尖峰整个丢失;dense 档(256,被 min 收敛到
    // 源上限 128)则能取到。即"8 倍高程细节留在 CPU 从未上 GPU"的可测形式。
    DecodedHeightmap heightmap;
    heightmap.tileSize = 129;
    heightmap.heights.assign(129 * 129, 0.0f);
    // 奇数格点(65,65):coarse 的节点在偶数格,取不到;dense 的节点覆盖每个源像素。
    heightmap.heights[static_cast<size_t>(65) * 129 + 65] = 400.0f;
    const Rectangle bounds = rootBounds();

    const double lon = bounds.west() + bounds.width() * (65.0 / 128.0);
    const double lat = bounds.north() - bounds.height() * (65.0 / 128.0);

    const float coarse = DecodedHeightmapSampler::sampleHeightRenderGrid(
        heightmap, bounds, lon, lat, kTerrainDisplacementGridSize);
    const float dense = DecodedHeightmapSampler::sampleHeightRenderGrid(
        heightmap, bounds, lon, lat, kTerrainDenseGridSize);

    EXPECT_NEAR(400.0f, dense, 1.0f);        // dense:尖峰还原
    EXPECT_LT(coarse, 250.0f);               // coarse:被节点间线性插值抹掉大半
    EXPECT_GT(dense, coarse + 100.0f);
}
