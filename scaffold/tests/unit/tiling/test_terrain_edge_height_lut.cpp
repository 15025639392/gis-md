// 无缝北极星 ①-1:边高度 LUT 的索引约定与编码精度。
//
// 这里**不测**"邻居高度算得对不对"—— 那走的是 sampleHeightRenderGrid 单一
// 事实源,再在测试里搭一份等价实现就是自带被测逻辑的复制品(踩过)。这里测的
// 是两件只属于 LUT 自己、且错了会静默的事:
//   ① 表的索引约定必须与 shader 的取值约定逐一对应(节点数、j0/j1、插值系数);
//   ② 16bit 归一化编码在真实高程量程下的精度必须优于 Terrain-RGB 的 0.1m
//      编码步长,否则修 ε 的同时引入一个同量级的新误差。

#include "../../../src/earth_engine/tiling/TerrainEdgeHeightLut.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

using earth_engine::TerrainEdgeHeightLut;

static int gFailures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++gFailures;
    }
}

static void checkNear(double a, double b, double tol, const char* what) {
    if (!(std::fabs(a - b) <= tol)) {
        std::printf("FAIL: %s (%.6f vs %.6f, tol %.6f)\n", what, a, b, tol);
        ++gFailures;
    }
}

// ① 节点数 = gridN/2^lg + 1。吸附步长越大节点越少;lg=0(不吸附)返回 0。
static void testNodeCount() {
    using earth_engine::terrain_edge::edgeNodeCount;
    check(edgeNodeCount(64, 0) == 0, "lg=0 不吸附 → 无节点");
    check(edgeNodeCount(64, 1) == 33, "coarse 邻粗一档 → 33 节点");
    check(edgeNodeCount(64, 2) == 17, "coarse 邻粗两档 → 17 节点");
    check(edgeNodeCount(256, 1) == 129, "dense 邻粗一档 → 129 节点(表宽上界)");
    check(edgeNodeCount(256, 1) <= TerrainEdgeHeightLut::kMaxNodes,
          "最密情形不得越过表宽");
}

// ② 表索引必须与 shader 的 a0/a1 取法对齐。逐格扫一遍整条边,任何一个顶点
//    落到表外或落错格,吸附就是把错位换个地方。
static void testShaderNodePairAlignment() {
    const int grid = 64;
    const int lg = 2;                      // step = 4
    const int nodes = earth_engine::terrain_edge::edgeNodeCount(grid, lg);
    for (int a = 0; a <= grid; ++a) {
        int j0 = -1, j1 = -1;
        float frac = -1.0f;
        TerrainEdgeHeightLut::shaderNodePair(static_cast<float>(a), lg, grid,
                                             j0, j1, frac);
        check(j0 >= 0 && j0 < nodes, "j0 落在表内");
        check(j1 >= 0 && j1 < nodes, "j1 落在表内");
        check(j1 >= j0 && j1 - j0 <= 1, "j1 = j0 或 j0+1");
        check(frac >= 0.0f && frac <= 1.0f, "插值系数落在 [0,1]");
        // 节点位置上必须精确落格(frac=0),否则线性插值会从错误的一对节点起算。
        if (a % (1 << lg) == 0) {
            checkNear(frac, 0.0, 1e-6, "节点位置 frac 必须为 0");
            checkNear(j0, a >> lg, 0, "节点位置 j0 = a/step");
        }
    }
    // 末端:a=gridN 时 a1 被 clamp 到 gridN,j0=j1=最后一个节点,不得越界。
    int j0 = 0, j1 = 0;
    float frac = 0.0f;
    TerrainEdgeHeightLut::shaderNodePair(static_cast<float>(grid), lg, grid, j0,
                                         j1, frac);
    check(j0 == nodes - 1 && j1 == nodes - 1, "末端两节点重合于表尾");
}

// ③ 编码精度。真实量程:重庆山区一条边的起伏跨度按 2000m 算(远超实测),
//    16bit 归一化的量化步长必须显著小于 Terrain-RGB 的 0.1m 编码步长 ——
//    否则修掉 ε 的同时引入一个同量级的新误差,白做。
static void testEncodePrecision() {
    const float minH = -50.0f;
    const float range = 2000.0f;
    const float step = range / 65535.0f;
    check(step < 0.05f, "量化步长须显著优于 0.1m 编码步长");
    double worst = 0.0;
    for (int i = 0; i <= 1000; ++i) {
        const float h = minH + range * (static_cast<float>(i) / 1000.0f);
        const uint16_t q = TerrainEdgeHeightLut::encode(h, minH, range);
        const float back = TerrainEdgeHeightLut::decode(q, minH, range);
        worst = std::max(worst, std::fabs(static_cast<double>(back - h)));
    }
    check(worst <= step, "往返误差不超过一个量化步长");
}

// ④ 平坦边(整条边等高)是完全正常的输入,range=0 会让归一化除零。
static void testFlatEdgeGuard() {
    const float minH = 500.0f;
    const uint16_t q = TerrainEdgeHeightLut::encode(500.0f, minH, 0.0f);
    const float back = TerrainEdgeHeightLut::decode(q, minH, 0.0f);
    check(std::isfinite(back), "平坦边编码不得产出非有限值");
    checkNear(back, 500.0, 1e-3, "平坦边往返仍得回原值");
}

// ⑤ 空记录:没有 tile / 档位非法时必须给出"全边不吸附",而不是半张表。
static void testEmptyRecord() {
    earth_engine::TileEdgeSnapRecord rec;
    const TerrainEdgeHeightLut::Data d = TerrainEdgeHeightLut::build(rec, 64);
    check(!d.hasAny(), "空记录不得产出任何边");
    check(d.filledEdges() == 0, "空记录 filledEdges = 0");
    check(d.heightRange >= TerrainEdgeHeightLut::kMinRange,
          "range 恒有下限保护");
}

int main() {
    testNodeCount();
    testShaderNodePairAlignment();
    testEncodePrecision();
    testFlatEdgeGuard();
    testEmptyRecord();
    if (gFailures == 0) {
        std::printf("test_terrain_edge_height_lut: 全部通过\n");
        return EXIT_SUCCESS;
    }
    std::printf("test_terrain_edge_height_lut: %d 处失败\n", gFailures);
    return EXIT_FAILURE;
}
