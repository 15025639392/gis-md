// 层间契约(debug/Contracts.h)的机制自检 + 边样板的判别力测试。
//
// 为什么这些测试必须存在:一条**永远不会触发**的契约,和一条**每次都通过**的契约,
// 在日志里长得一模一样(都是没有输出)。所以每条边都要配一对控制组——正例不响、
// 反例响——否则契约本身成了新的不可观测物,把 A/B 的问题原样搬了个家。
#include <gtest/gtest.h>

#include "earth_engine/content/GltfTerrainUpsampler.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/debug/Contracts.h"
#include "earth_engine/renderer/TerrainInstanceBatcher.h"

#include <algorithm>
#include <string>
#include <vector>

namespace earth_engine {
namespace {

// 计数器是进程级单调量,没有(也不该有)测试专用的清零接口——生产代码不该为测试
// 开后门。所有断言一律比**增量**,因此与测试执行顺序无关。
uint32_t delta(contracts::Id id, uint32_t before) {
    return contracts::totalViolations(id) - before;
}

// ---- 机制自检 ----

TEST(Contracts, PassingConditionRecordsNothing) {
    const uint32_t before =
        contracts::totalViolations(contracts::Id::DemNodataSentinel);
    GE_CONTRACT(contracts::Id::DemNodataSentinel, 1 == 1, "should not fire");
    EXPECT_EQ(0u, delta(contracts::Id::DemNodataSentinel, before));
}

TEST(Contracts, FailingConditionIncrementsTotal) {
    const uint32_t before =
        contracts::totalViolations(contracts::Id::DemNodataSentinel);
    GE_CONTRACT(contracts::Id::DemNodataSentinel, 1 == 2, "x=%d", 42);
    EXPECT_EQ(1u, delta(contracts::Id::DemNodataSentinel, before));
}

TEST(Contracts, RepeatedViolationsAllCount) {
    // 首违约之后只计数不打日志,但**计数不能停** —— 否则"违了 3 次"和"违了
    // 30000 次"无法区分,而这正是判暂态/稳态的唯一依据。
    const uint32_t before =
        contracts::totalViolations(contracts::Id::DemNodataSentinel);
    for (int i = 0; i < 5; ++i) {
        GE_CONTRACT(contracts::Id::DemNodataSentinel, false, "i=%d", i);
    }
    EXPECT_EQ(5u, delta(contracts::Id::DemNodataSentinel, before));
}

TEST(Contracts, ConditionIsEvaluatedExactlyOnce) {
    // 宏把 cond 展开进 if,带副作用的表达式重复求值会静默改变被诊断的行为。
    const uint32_t before =
        contracts::totalViolations(contracts::Id::TexcoordNwOrigin);
    int evaluations = 0;
    GE_CONTRACT(contracts::Id::TexcoordNwOrigin,
                (++evaluations, false), "eval=%d", evaluations);
    EXPECT_EQ(1, evaluations);
    EXPECT_EQ(1u, delta(contracts::Id::TexcoordNwOrigin, before));
}

TEST(Contracts, FrameSummaryClearsPerFrameButKeepsTotal) {
    const uint32_t before =
        contracts::totalViolations(contracts::Id::TexcoordNwOrigin);
    GE_CONTRACT(contracts::Id::TexcoordNwOrigin, false, "before summary");
    contracts::logFrameSummary(1);          // 清零逐帧计数
    contracts::logFrameSummary(2);          // 已清零 → 全绿,不打
    EXPECT_EQ(1u, delta(contracts::Id::TexcoordNwOrigin, before));
}

TEST(Contracts, EveryIdHasAName) {
    // 名字表与枚举必须等长:漏一个会让日志报 "?" ——出问题时最不该发生的事。
    for (uint8_t i = 0; i < static_cast<uint8_t>(contracts::Id::Count); ++i) {
        const char* n = contracts::name(static_cast<contracts::Id>(i));
        ASSERT_NE(nullptr, n);
        EXPECT_STRNE("?", n) << "contracts::Id 序号 " << int(i) << " 没有名字";
    }
}

// ---- 边 3:TexcoordNwOrigin ----

SurfaceVertex surfaceVertexAt(double lonDeg, double latDeg, float u, float v) {
    SurfaceVertex out;
    out.positionEcef = Ellipsoid::WGS84().cartographicToCartesian(
        Cartographic::fromDegrees(lonDeg, latDeg, 0.0));
    out.normalEcef =
        Ellipsoid::WGS84().geodeticSurfaceNormal(out.positionEcef);
    out.uv = {u, v};
    return out;
}

// 一个四顶点的父网格。northV / southV 决定北缘、南缘各自拿到的 v 值,
// 从而可以按需构造出 NW(北=0)或翻转(北=1)两种父网格。
GltfModel makeParent(float northV, float southV) {
    GltfModel model;
    GltfPrimitive primitive;
    primitive.vertices = {
        surfaceVertexAt(10.0, 40.0, 0.0f, northV),   // 北西
        surfaceVertexAt(11.0, 40.0, 1.0f, northV),   // 北东
        surfaceVertexAt(10.0, 39.0, 0.0f, southV),   // 南西
        surfaceVertexAt(11.0, 39.0, 1.0f, southV)};  // 南东
    primitive.indices = {0, 1, 2, 1, 3, 2};
    primitive.vertexTexCoords[0] = {
        {0.0f, northV}, {1.0f, northV}, {0.0f, southV}, {1.0f, southV}};
    primitive.runtime.baseVertices = primitive.vertices;
    primitive.runtime.hasNormals = true;
    model.primitives.push_back(std::move(primitive));
    return model;
}

UpsampledQuadtreeNode childNode() {
    UpsampledQuadtreeNode child;
    child.tileID = TileKey{"XYZ-WebMercator", 5, 8, 6};
    return child;
}

TEST(ContractTexcoordNwOrigin, NwParentDoesNotViolate) {
    const uint32_t before =
        contracts::totalViolations(contracts::Id::TexcoordNwOrigin);
    // NW:北缘 v=0、南缘 v=1。
    const GltfModel parent = makeParent(/*northV=*/0.0f, /*southV=*/1.0f);
    GltfTerrainUpsampler::upsampleForRasterOverlay(
        parent, childNode(), 0, /*hasInvertedVCoordinate=*/false);
    EXPECT_EQ(0u, delta(contracts::Id::TexcoordNwOrigin, before));
}

TEST(ContractTexcoordNwOrigin, FlippedParentViolates) {
    const uint32_t before =
        contracts::totalViolations(contracts::Id::TexcoordNwOrigin);
    // 父网格按相反约定发 UV(北缘 v=1),但调用方仍声明 invertedV=false。
    // 这正是 6 处独立注释各自声明约定时,改一处漏一处的形态。
    const GltfModel parent = makeParent(/*northV=*/1.0f, /*southV=*/0.0f);
    GltfTerrainUpsampler::upsampleForRasterOverlay(
        parent, childNode(), 0, /*hasInvertedVCoordinate=*/false);
    EXPECT_EQ(1u, delta(contracts::Id::TexcoordNwOrigin, before));
}

TEST(ContractTexcoordNwOrigin, InvertedFlagAcceptsFlippedParent) {
    // 声明与数据一致时不该报警:契约管的是"两者是否一致",不是"必须 NW"。
    const uint32_t before =
        contracts::totalViolations(contracts::Id::TexcoordNwOrigin);
    const GltfModel parent = makeParent(/*northV=*/1.0f, /*southV=*/0.0f);
    GltfTerrainUpsampler::upsampleForRasterOverlay(
        parent, childNode(), 0, /*hasInvertedVCoordinate=*/true);
    EXPECT_EQ(0u, delta(contracts::Id::TexcoordNwOrigin, before));
}

TEST(ContractTexcoordNwOrigin, DegenerateVSpreadIsNotJudged) {
    // v 展布过小时方向没有信息量。与其在退化数据上给假阳性,不如沉默 ——
    // 假阳性会训练出"这条警告可以忽略"的习惯,那比没有警告更糟。
    const uint32_t before =
        contracts::totalViolations(contracts::Id::TexcoordNwOrigin);
    const GltfModel parent = makeParent(/*northV=*/0.50f, /*southV=*/0.51f);
    GltfTerrainUpsampler::upsampleForRasterOverlay(
        parent, childNode(), 0, /*hasInvertedVCoordinate=*/false);
    EXPECT_EQ(0u, delta(contracts::Id::TexcoordNwOrigin, before));
}

// ---- 覆盖计数:区分「边成立」与「判定点没跑到」 ----

TEST(ContractCoverage, PassingCheckStillCountsAsEvaluation) {
    // 这是覆盖计数存在的全部理由:只看违约数,一条死契约和一条永远成立的契约
    // 完全一样(都是 0)。求值计数让"这条边还活着"变成可读事实。
    const uint32_t before =
        contracts::totalEvaluations(contracts::Id::DemNodataSentinel);
    GE_CONTRACT(contracts::Id::DemNodataSentinel, true, "passing");
    EXPECT_EQ(1u, contracts::totalEvaluations(contracts::Id::DemNodataSentinel) -
                      before);
}

TEST(ContractCoverage, ViolationAlsoCountsAsEvaluation) {
    const uint32_t evalBefore =
        contracts::totalEvaluations(contracts::Id::DemNodataSentinel);
    const uint32_t violBefore =
        contracts::totalViolations(contracts::Id::DemNodataSentinel);
    GE_CONTRACT(contracts::Id::DemNodataSentinel, false, "failing");
    EXPECT_EQ(1u, contracts::totalEvaluations(contracts::Id::DemNodataSentinel) -
                      evalBefore);
    EXPECT_EQ(1u, contracts::totalViolations(contracts::Id::DemNodataSentinel) -
                      violBefore);
}

TEST(ContractCoverage, NeverExecutedEdgeStaysAtZero) {
    // 反过来的控制组:没有任何判定点跑过的边,求值数必须停在 0 —— 否则覆盖数
    // 就成了摆设,分不出死边。PageDecorateOrdering 在 host 测试里没有驱动路径,
    // 正好当这个反例(它在真机上由 coverage 行证明活性)。
    EXPECT_EQ(0u,
              contracts::totalEvaluations(contracts::Id::PageDecorateOrdering))
        << "若此断言失败,说明有 host 路径开始驱动这条边了 —— 那是好事,"
           "把这个测试改成正例即可。";
}

// ---- 边 2:BatchTemplateGridParity 的判据(脱离 GL 上下文单测) ----

TerrainInstanceBatcher::InstanceRecord recordWithGrid(float gridN) {
    TerrainInstanceBatcher::InstanceRecord rec{};
    rec.layers[3] = gridN;
    return rec;
}

TEST(ContractBatchTemplateGrid, UniformBatchPasses) {
    const auto records = std::vector<TerrainInstanceBatcher::InstanceRecord>{
        recordWithGrid(64.0f), recordWithGrid(64.0f), recordWithGrid(64.0f)};
    EXPECT_TRUE(TerrainInstanceBatcher::batchTemplateGridIsUniform(
        records.data(), records.size()));
}

TEST(ContractBatchTemplateGrid, MixedBatchFails) {
    // 分组规则一旦改成把不同 gridSize 的模板并进一批,就是这个形态:shader 会按
    // 错误的栅格边长解算格点位置,整批地形几何错位且没有任何报错。
    const auto records = std::vector<TerrainInstanceBatcher::InstanceRecord>{
        recordWithGrid(64.0f), recordWithGrid(64.0f), recordWithGrid(32.0f)};
    EXPECT_FALSE(TerrainInstanceBatcher::batchTemplateGridIsUniform(
        records.data(), records.size()));
    // 违约现场要能指出**是哪个档位**混进来了,否则首违约日志没有诊断价值。
    EXPECT_FLOAT_EQ(32.0f,
                    TerrainInstanceBatcher::firstMismatchedTemplateGrid(
                        records.data(), records.size()));
}

TEST(ContractBatchTemplateGrid, DegenerateInputsAreNotJudged) {
    // 空批/单实例批没有可比对象。在退化输入上报警只会训练出"这条警告可以忽略"
    // 的习惯,那会一并废掉旁边真正有效的契约。
    const auto single = std::vector<TerrainInstanceBatcher::InstanceRecord>{
        recordWithGrid(64.0f)};
    EXPECT_TRUE(TerrainInstanceBatcher::batchTemplateGridIsUniform(
        single.data(), single.size()));
    EXPECT_TRUE(
        TerrainInstanceBatcher::batchTemplateGridIsUniform(nullptr, 0));
    EXPECT_FLOAT_EQ(
        -1.0f, TerrainInstanceBatcher::firstMismatchedTemplateGrid(nullptr, 0));
}

// ---- 名字表完整性(边增加时最容易漏的一步) ----

TEST(Contracts, NameTableCoversEveryId) {
    // kNames 与枚举等长由 Contracts.cpp 的 static_assert 保证;这里再查内容不为
    // 空且互不相同 —— 复制粘贴新增枚举时最常见的是名字忘了改。
    std::vector<std::string> seen;
    for (uint8_t i = 0; i < static_cast<uint8_t>(contracts::Id::Count); ++i) {
        const std::string n = contracts::name(static_cast<contracts::Id>(i));
        EXPECT_FALSE(n.empty());
        EXPECT_EQ(seen.end(), std::find(seen.begin(), seen.end(), n))
            << "契约名重复: " << n;
        seen.push_back(n);
    }
}

}  // namespace
}  // namespace earth_engine
