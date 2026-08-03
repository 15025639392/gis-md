#include <gtest/gtest.h>

#include "earth_engine/tiling/TileEdgeSnapResolver.h"
#include "earth_engine/tiling/TileScheme.h"

#include <deque>

using namespace earth_engine;

namespace {

// 无缝北极星机制 B 的 CPU 侧契约:从渲染集解析 4 边邻居八度差并打包。
// 打包序(shader 解码同一约定):W + 8·E + 64·N + 512·S,N = v0(北)边。
struct PlanFixture {
    TilePlan plan;
    std::deque<TilesetTile> tiles;  // deque:指针稳定
    SchemeId scheme = SchemeId("Test-Scheme");

    TilesetTile& addEntry(int z, int x, int y, double sse = 10.0,
                          bool fallback = false, int ancestorZ = 0) {
        tiles.emplace_back();
        TilesetTile& t = tiles.back();
        t.key = TileKey{scheme, z, x, y};
        t.selectionFrameState.screenSpaceError = sse;
        TileRenderEntry e;
        e.selectedKey = t.key;
        e.renderKey = fallback ? TileKey{scheme, ancestorZ, x >> (z - ancestorZ),
                                         y >> (z - ancestorZ)}
                               : t.key;
        e.selectedThisFrame = true;
        e.usesAncestorFallback = fallback;
        e.selectedTile = &t;
        e.renderTile = &t;  // 测试不关心 renderTile 指向
        plan.renderEntries.push_back(e);
        return t;
    }

    float snapOf(const TilesetTile& t) const {
        return t.selectionFrameState.edgeSnapPacked;
    }
};

} // namespace

// 同级同档邻居:无差,四边全 0。
TEST(TileEdgeSnapResolverTest, SameLevelNeighborsNoSnap) {
    PlanFixture f;
    TilesetTile& a = f.addEntry(10, 4, 4);
    f.addEntry(10, 3, 4);
    f.addEntry(10, 5, 4);
    f.addEntry(10, 4, 3);
    f.addEntry(10, 4, 5);
    TileEdgeSnapResolver::resolve(f.plan);
    EXPECT_EQ(0.0f, f.snapOf(a));
}

// 西邻粗一级(z9 覆盖 z10 的 x=3 cell):W=1,其余边无邻居 → 0。
TEST(TileEdgeSnapResolverTest, CoarserWestNeighborSnapsWestOnly) {
    PlanFixture f;
    TilesetTile& a = f.addEntry(10, 4, 4);
    f.addEntry(9, 1, 2);  // z9 (1,2) 覆盖 z10 (2..3, 4..5) → 含西邻 (3,4)
    TileEdgeSnapResolver::resolve(f.plan);
    EXPECT_EQ(1.0f, f.snapOf(a));  // W=1
}

// 北邻粗两级:N=2 → 打包 64·2 = 128。y 轴约定:v0=北 = y-1。
TEST(TileEdgeSnapResolverTest, CoarserNorthNeighborTwoLevels) {
    PlanFixture f;
    TilesetTile& a = f.addEntry(10, 4, 4);
    f.addEntry(8, 1, 0);  // z8 (1,0) 覆盖 z10 (4..7, 0..3) → 含北邻 (4,3)
    TileEdgeSnapResolver::resolve(f.plan);
    EXPECT_EQ(128.0f, f.snapOf(a));
}

// 同 z 异档:本瓦片 dense(+2 八度),邻居 coarse → 邻居向我?不:我更细,
// **我**向邻居吸 2 级。dense 判定走 terrainGridSizeForSse(SSE 超阈值)。
TEST(TileEdgeSnapResolverTest, TierMismatchSameLevel) {
    PlanFixture f;
    TilesetTile& dense = f.addEntry(
        10, 4, 4, kTerrainDenseGridSseThresholdPixels + 1.0);
    f.addEntry(10, 3, 4, 10.0);  // 西邻 coarse
    TileEdgeSnapResolver::resolve(f.plan);
    EXPECT_EQ(2.0f, f.snapOf(dense));       // W=2(档差)
    EXPECT_EQ(0.0f, f.snapOf(f.tiles[1]));  // 粗侧不吸
}

// 邻居更细(渲染其子级):本级探不到 → 0(细侧负责吸)。
TEST(TileEdgeSnapResolverTest, FinerNeighborNoSnapOnCoarseSide) {
    PlanFixture f;
    TilesetTile& a = f.addEntry(10, 4, 4);
    f.addEntry(11, 6, 8);  // z11 (6,8) = z10 (3,4) 的西邻子级
    f.addEntry(11, 6, 9);
    f.addEntry(11, 7, 8);
    f.addEntry(11, 7, 9);
    TileEdgeSnapResolver::resolve(f.plan);
    EXPECT_EQ(0.0f, f.snapOf(a));
}

// 祖先回退(remap)条目:按数据八度(renderKey.z)参与索引 → 细邻居向它
// 按 z 差吸;remap 自身恒 0。
TEST(TileEdgeSnapResolverTest, RemapEntryIndexedAtAncestorOctave) {
    PlanFixture f;
    TilesetTile& fine = f.addEntry(10, 4, 4);
    TilesetTile& remap = f.addEntry(10, 3, 4, 10.0, true, 7);  // 数据来自 z7
    TileEdgeSnapResolver::resolve(f.plan);
    EXPECT_EQ(3.0f, f.snapOf(fine));   // W = 10-7 = 3
    EXPECT_EQ(0.0f, f.snapOf(remap));  // remap 不吸
}

// 经向环绕:x=0 的西邻 = x=2^z-1。
TEST(TileEdgeSnapResolverTest, LongitudeWrap) {
    PlanFixture f;
    TilesetTile& a = f.addEntry(10, 0, 4);
    f.addEntry(9, 511, 2);  // 覆盖 z10 x=1022..1023 → 含环绕西邻 (1023,4)
    TileEdgeSnapResolver::resolve(f.plan);
    EXPECT_EQ(1.0f, f.snapOf(a));
}

// 步长上限:z 差 8 clamp 到 6(coarse 64 格步长上限 2^6)。
TEST(TileEdgeSnapResolverTest, DeltaClampedToSix) {
    PlanFixture f;
    TilesetTile& a = f.addEntry(12, 256, 256);
    f.addEntry(4, 0, 1);  // z4 (0,1) 覆盖 z12 (0..255, 256..511) → 西邻域
    TileEdgeSnapResolver::resolve(f.plan);
    EXPECT_EQ(6.0f, f.snapOf(a));
}
