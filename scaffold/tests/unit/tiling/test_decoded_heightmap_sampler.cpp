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
    heightmap.stagedHeights = {42.0f, 42.0f, 42.0f, 42.0f};
    heightmap.assignHeights();
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
    heightmap.stagedHeights = {30.0f, 40.0f, 10.0f, 20.0f};
    heightmap.assignHeights();
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
    heightmap.stagedHeights = {50.0f, 50.0f, 50.0f, 50.0f};
    heightmap.assignHeights();
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
    heightmap.stagedHeights = {65535.0f, 65535.0f, 65535.0f, 65535.0f};
    heightmap.assignHeights();
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
    west.stagedHeights.clear();
    for (int r = 0; r < 4; ++r)
        for (float x : {-0.5f, 0.5f, 1.5f, 2.5f}) west.stagedHeights.push_back(x);
    west.assignHeights();
    // 东片覆盖世界 [2,4]:内部 cell 中心 x=2.5,3.5;重叠列 x=1.5(西邻),4.5(东邻)
    DecodedHeightmap east;
    east.tileSize = 4;
    east.borderInset = 0.5f;
    east.stagedHeights.clear();
    for (int r = 0; r < 4; ++r)
        for (float x : {1.5f, 2.5f, 3.5f, 4.5f}) east.stagedHeights.push_back(x);

    east.assignHeights();
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
    hm.stagedHeights = {0, 10, 20, 0, 10, 20, 0, 10, 20};  // 每行 0,10,20
    hm.assignHeights();
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
    heightmap.stagedHeights.assign(129 * 129, 0.0f);
    heightmap.stagedHeights[static_cast<size_t>(65) * 129 + 65] = 500.0f;  // 奇数格
    heightmap.assignHeights();
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
    heightmap.stagedHeights.assign(129 * 129, 0.0f);
    heightmap.stagedHeights[static_cast<size_t>(64) * 129 + 64] = 300.0f;  // 偶数格=节点
    heightmap.assignHeights();
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
    // 源 ≤ 65 时节点集合相同；对平面高度场，双线性源采样与 GPU 两三角形
    // 插值恒等。非平面 cell 由下一条测试锁定为 GPU 拓扑，而非双线性 patch。
    DecodedHeightmap heightmap;
    heightmap.tileSize = 5;
    heightmap.stagedHeights.resize(25);
    for (int y = 0; y < 5; ++y) {
        for (int x = 0; x < 5; ++x) {
            heightmap.stagedHeights[static_cast<size_t>(y) * 5 + x] =
                static_cast<float>(x * 10 + y * 20);
        }
    }
    heightmap.assignHeights();
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

TEST(DecodedHeightmapSamplerTest,
     RenderGridInterpolatesGpuTrianglesInsteadOfBilinearPatch) {
    DecodedHeightmap heightmap;
    heightmap.tileSize = 2;
    heightmap.stagedHeights = {0.0f, 0.0f, 0.0f, 100.0f};
    heightmap.assignHeights();
    heightmap.minHeight = 0.0f;
    heightmap.maxHeight = 100.0f;
    const Rectangle bounds = rootBounds();
    const double lon = bounds.west() + bounds.width() * 0.5;
    const double lat = bounds.north() - bounds.height() * 0.5;

    EXPECT_FLOAT_EQ(
        0.0f,
        DecodedHeightmapSampler::sampleHeightRenderGrid(
            heightmap, bounds, lon, lat, 1))
        << "the fixed a-c-b / b-c-d diagonal crosses zero-height nodes; "
           "a bilinear patch would incorrectly return 25";
    EXPECT_FLOAT_EQ(
        0.0f,
        DecodedHeightmapSampler::sampleHeightRenderGridQuantized(
            heightmap, bounds, lon, lat, 1, 0.0f, 100.0f));
}

// ============================================================
// 自适应几何密度(解 65×65 钉死)
// ============================================================

TEST(TerrainGridSizeForSseTest, UpgradesOnlyAboveThresholdAndHasHysteresis) {
    // 多档表(kGridTiers):base 64 / mid 128 / dense 256。
    const GridTier& base = kGridTiers[0];
    const GridTier& mid = kGridTiers[1];
    const GridTier& dense = kGridTiers[2];
    // 未定档(0):按各档 acquire 阈值升到最高可达档。
    EXPECT_EQ(base.gridSize, terrainGridSizeForSse(mid.acquireSsePx - 0.1, 0));
    EXPECT_EQ(mid.gridSize, terrainGridSizeForSse(mid.acquireSsePx, 0));
    EXPECT_EQ(dense.gridSize, terrainGridSizeForSse(dense.acquireSsePx, 0));

    // 已在 mid 档:SSE 低于 mid.release → 降回 base;达 dense.acquire → 升 dense。
    EXPECT_EQ(base.gridSize,
              terrainGridSizeForSse(mid.releaseSsePx - 0.1, mid.gridSize));
    EXPECT_EQ(mid.gridSize,
              terrainGridSizeForSse(mid.releaseSsePx, mid.gridSize));
    EXPECT_EQ(dense.gridSize,
              terrainGridSizeForSse(dense.acquireSsePx, mid.gridSize));

    // 已在 dense 档:SSE 低于 dense.release → 降一级到 mid。
    EXPECT_EQ(mid.gridSize,
              terrainGridSizeForSse(dense.releaseSsePx - 0.1, dense.gridSize));
    EXPECT_EQ(dense.gridSize,
              terrainGridSizeForSse(dense.releaseSsePx, dense.gridSize));

    // 迟滞:同一 SSE(mid 迟滞带 [mid.release, mid.acquire) 内)在不同档下结果不同
    // —— 缺迟滞带会让 SSE 在阈值附近抖动的瓦片逐帧换档。
    const double inMidBand = (mid.releaseSsePx + mid.acquireSsePx) * 0.5;
    EXPECT_EQ(base.gridSize, terrainGridSizeForSse(inMidBand, base.gridSize));
    EXPECT_EQ(mid.gridSize, terrainGridSizeForSse(inMidBand, mid.gridSize));
    EXPECT_NE(terrainGridSizeForSse(inMidBand, base.gridSize),
              terrainGridSizeForSse(inMidBand, mid.gridSize));
}

// 跨档 morph:最低档交 LOD morph(返回 1);密档在 acquire 处为 0(长得像上一
// 档),随 SSE 在 ramp 宽度内推进到 1(全细节)。单调、有界。
TEST(TerrainGridSizeForSseTest, CrossTierMorphRampsWithinTierBand) {
    const GridTier& base = kGridTiers[0];
    const GridTier& mid = kGridTiers[1];
    const GridTier& dense = kGridTiers[2];
    // 最低档:无更粗态 → 恒 1(交 LOD morph)。
    EXPECT_EQ(1.0f, terrainTierMorphForSse(0.0, base.gridSize));
    EXPECT_EQ(1.0f, terrainTierMorphForSse(100.0, base.gridSize));
    // mid 档:acquire 处 0,ramp 宽度后 1,中间单调。
    EXPECT_EQ(0.0f, terrainTierMorphForSse(mid.acquireSsePx, mid.gridSize));
    EXPECT_EQ(1.0f, terrainTierMorphForSse(
                        mid.acquireSsePx + kTerrainTierMorphRampSsePx,
                        mid.gridSize));
    const float half =
        terrainTierMorphForSse(mid.acquireSsePx + kTerrainTierMorphRampSsePx * 0.5,
                               mid.gridSize);
    EXPECT_GT(half, 0.0f);
    EXPECT_LT(half, 1.0f);
    // dense 档:同样从 acquire 处 0 爬到 ramp 后 1。
    EXPECT_EQ(0.0f, terrainTierMorphForSse(dense.acquireSsePx, dense.gridSize));
    EXPECT_EQ(1.0f, terrainTierMorphForSse(
                        dense.acquireSsePx + kTerrainTierMorphRampSsePx,
                        dense.gridSize));
}

TEST(DecodedHeightmapSamplerTest, DenseGridResolvesDetailCoarseGridMisses) {
    // 这条锁死本次改动的**目的**:coarse 档(64 格)在 129 源上每 2 个源像素才
    // 取一个节点,落在奇数源像素上的尖峰整个丢失;dense 档(256,被 min 收敛到
    // 源上限 128)则能取到。即"8 倍高程细节留在 CPU 从未上 GPU"的可测形式。
    DecodedHeightmap heightmap;
    heightmap.tileSize = 129;
    heightmap.stagedHeights.assign(129 * 129, 0.0f);
    // 奇数格点(65,65):coarse 的节点在偶数格,取不到;dense 的节点覆盖每个源像素。
    heightmap.stagedHeights[static_cast<size_t>(65) * 129 + 65] = 400.0f;
    heightmap.assignHeights();
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

// ============================================================
// 无缝北极星 P4:同级邻接边一致性(机制 B「边吸附」的地基假设)
// docs/issues/terrain-seamless-northstar-2026-08-03.md §4-P4
// ============================================================

namespace {

// 确定性伪随机世界高度场:纯世界坐标函数 → 邻接瓦片重叠环自动一致,
// 正如真实源数据(同一 DEM 切出来的相邻瓦片)。振幅 ~±1200m 模拟山地。
float worldHeight(double wx, double wy) {
    const double a = std::sin(wx * 0.0173) * 700.0 +
                     std::sin(wy * 0.0311 + 1.7) * 400.0 +
                     std::sin((wx + wy) * 0.0059) * 900.0 +
                     std::sin(wx * 0.131) * std::sin(wy * 0.097) * 180.0;
    return static_cast<float>(a);
}

// Mapbox 514 cell-registered + 1px 重叠环瓦片:瓦片 (tx,ty) 拥有 512 格,
// 像素 p∈[1,512] = 世界格 t*512+p-1 的中心;像素 0/513 = 邻居重叠 backfill。
// 统一公式 worldCoord(p) = t*512 + (p-1) + 0.5 对 p∈[0,513] 同时给出两者。
DecodedHeightmap makeOverlapTile514(int tx, int ty) {
    DecodedHeightmap hm;
    hm.tileSize = 514;
    hm.borderInset = 0.5f;
    hm.stagedHeights.resize(514 * 514);
    float mn = 1e9f, mx = -1e9f;
    for (int py = 0; py < 514; ++py) {
        const double wy = ty * 512.0 + (py - 1) + 0.5;
        for (int px = 0; px < 514; ++px) {
            const double wx = tx * 512.0 + (px - 1) + 0.5;
            const float h = worldHeight(wx, wy);
            hm.stagedHeights[static_cast<size_t>(py) * 514 + px] = h;
            mn = std::min(mn, h);
            mx = std::max(mx, h);
        }
    }
    hm.assignHeights();
    hm.minHeight = mn;
    hm.maxHeight = mx;
    return hm;
}

} // namespace

// 同级东西邻接:西片 u=1 整条边与东片 u=0 整条边在渲染栅格全部节点上
// **逐位相等**(EXPECT_EQ,不是 NEAR)。这是边吸附能不做任何邻居采样、
// 直接信任"同级边天然一致"的前提;此前只有一次性真机对拍(maxdiff=0),
// 没有锁进测试。coarse(65 节点)与 dense(257 节点)两档都锁。
TEST(SeamNorthstarP4Test, SameLevelEastWestEdgeBitwiseEqual) {
    const DecodedHeightmap west = makeOverlapTile514(0, 0);
    const DecodedHeightmap east = makeOverlapTile514(1, 0);
    for (int gridSize : {kTerrainDisplacementGridSize, kTerrainDenseGridSize}) {
        const int n = gridSize + 1;
        for (int j = 0; j < n; ++j) {
            const float v = static_cast<float>(j) / static_cast<float>(gridSize);
            const float hw = west.sampleBilinear(1.0f, v);
            const float he = east.sampleBilinear(0.0f, v);
            ASSERT_EQ(hw, he) << "grid=" << gridSize << " j=" << j;
        }
    }
}

// 同级南北邻接:对称锁另一个方向(采样代码 x/y 路径独立,东西过不代表南北过)。
TEST(SeamNorthstarP4Test, SameLevelNorthSouthEdgeBitwiseEqual) {
    const DecodedHeightmap north = makeOverlapTile514(0, 0);
    const DecodedHeightmap south = makeOverlapTile514(0, 1);
    for (int gridSize : {kTerrainDisplacementGridSize, kTerrainDenseGridSize}) {
        const int n = gridSize + 1;
        for (int i = 0; i < n; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(gridSize);
            const float hn = north.sampleBilinear(u, 1.0f);  // 北片南边界
            const float hs = south.sampleBilinear(u, 0.0f);  // 南片北边界
            ASSERT_EQ(hn, hs) << "grid=" << gridSize << " i=" << i;
        }
    }
}

// 量化基分歧上界:GPU 高度纹理是 16bit 归一化,minH/range **逐瓦片**
// (TerrainDisplacementTemplatePool::acquireHeightTexture)。相邻瓦片量化基不同,
// 同一物理高度往返 encode/decode 后有分歧 —— 这是源一致性锁不住的最后一段。
// 锁两条:①分歧 ≤ 双方量化步长之和的一半(理论界);②山地量级 range 下
// 绝对值 ≤ 0.1m(不可见,边吸附无需为它做任何补偿)。
TEST(SeamNorthstarP4Test, PerTileQuantizationDivergenceBounded) {
    const DecodedHeightmap west = makeOverlapTile514(0, 0);
    const DecodedHeightmap east = makeOverlapTile514(1, 0);
    const auto quantRoundTrip = [](const DecodedHeightmap& hm, float h) {
        const float range = std::max(1e-3f, hm.maxHeight - hm.minHeight);
        const float t = std::clamp((h - hm.minHeight) / range, 0.0f, 1.0f);
        const uint32_t v16 = static_cast<uint32_t>(std::lround(t * 65535.0f));
        // shader 解码:(R*256+G)/65535 * range + minH(见 eeSampleTerrainHeight)
        return hm.minHeight + (static_cast<float>(v16) / 65535.0f) * range;
    };
    const float stepW = (west.maxHeight - west.minHeight) / 65535.0f;
    const float stepE = (east.maxHeight - east.minHeight) / 65535.0f;
    const float bound = 0.5f * (stepW + stepE) + 1e-4f;
    float worst = 0.0f;
    const int n = kTerrainDenseGridSize + 1;
    for (int j = 0; j < n; ++j) {
        const float v = static_cast<float>(j) / kTerrainDenseGridSize;
        const float h = west.sampleBilinear(1.0f, v);  // 已证与东片逐位相等
        const float dw = quantRoundTrip(west, h);
        const float de = quantRoundTrip(east, h);
        worst = std::max(worst, std::abs(dw - de));
    }
    EXPECT_LE(worst, bound);
    EXPECT_LE(worst, 0.1f);  // 山地量级下绝对不可见
}

// === 无缝北极星根修回归:nodata 重叠环(全球金字塔缺邻居时环列整条 -10000,
// 实测 6/51/25 西环形态)。哨兵未注册时 -10000 被当合法高度混进边缘双线性 →
// 边缘一格宽 km 级假深沟(顶点被紧 near/far 裁掉 = 黑裂缝)+假悬崖法线
// (瓦片边界光照条带)。锁两条:①注册哨兵后环 nodata 被 renormalize 剔除,
// 边缘采样不含哨兵渗漏;②四角全 nodata 原样回传哨兵,isNoData(result) 可判
// (烘焙侧据此落 0)。
TEST(SeamNorthstarNoDataTest, NoDataRingExcludedFromEdgeBilinear) {
    DecodedHeightmap hm = makeOverlapTile514(0, 0);
    hm.noDataValues.push_back(-10000.0f);
    for (int py = 0; py < 514; ++py) {
        hm.stagedHeights[static_cast<size_t>(py) * 514 + 0] = -10000.0f;
        hm.assignHeights();
    }
    for (int j = 0; j <= 64; ++j) {
        const float v = static_cast<float>(j) / 64.0f;
        const float h = hm.sampleBilinear(0.0f, v);
        EXPECT_FALSE(hm.isNoData(h));
        EXPECT_GT(h, -1000.0f);
    }
}

TEST(SeamNorthstarNoDataTest, AllNoDataCornersPropagateSentinel) {
    DecodedHeightmap hm;
    hm.tileSize = 2;
    hm.stagedHeights = {-10000.0f, -10000.0f, -10000.0f, -10000.0f};
        hm.assignHeights();
    hm.noDataValues.push_back(-10000.0f);
    EXPECT_TRUE(hm.isNoData(hm.sampleBilinear(0.5f, 0.5f)));
}
