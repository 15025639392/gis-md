#include <gtest/gtest.h>

#include "earth_engine/data/FeatureSpatialIndex.h"
#include "earth_engine/core/math/Rectangle.h"

#include <algorithm>
#include <random>
#include <set>
#include <vector>

using namespace earth_engine;

namespace {

std::set<FeatureId> toSet(const std::vector<FeatureId>& v) {
    return std::set<FeatureId>(v.begin(), v.end());
}

// 裸平面 AABB 相交(含边界),与 FeatureSpatialIndex 内部判据一致。
// 不用 Rectangle::intersects —— 后者反经线感知 + 边界排斥,与索引契约不符。
bool rawIntersects(const Rectangle& a, const Rectangle& b) {
    return !(a.east() < b.west() || a.west() > b.east() ||
             a.north() < b.south() || a.south() > b.north());
}

// 暴力:线性扫描所有 (id, rect),返回与 q 相交的 id 集合。
// 与 index 用同一裸判据,对拍的是 R-tree 结构不漏不误。
std::set<FeatureId> bruteForce(
    const std::vector<std::pair<FeatureId, Rectangle>>& items,
    const Rectangle& q) {
    std::set<FeatureId> out;
    for (const auto& it : items) {
        if (rawIntersects(it.second, q)) out.insert(it.first);
    }
    return out;
}

} // namespace

// ============================================================
// 基础
// ============================================================

TEST(FeatureSpatialIndexTest, EmptyQueryReturnsEmpty) {
    FeatureSpatialIndex idx;
    EXPECT_TRUE(idx.empty());
    EXPECT_EQ(0u, idx.size());
    auto r = idx.query(Rectangle(-1, -1, 1, 1));
    EXPECT_TRUE(r.empty());
}

TEST(FeatureSpatialIndexTest, SingleInsertHitAndMiss) {
    FeatureSpatialIndex idx;
    idx.insert(42, Rectangle(0.0, 0.0, 1.0, 1.0));
    EXPECT_EQ(1u, idx.size());

    auto hit = idx.query(Rectangle(0.5, 0.5, 2.0, 2.0));
    ASSERT_EQ(1u, hit.size());
    EXPECT_EQ(42u, hit[0]);

    auto miss = idx.query(Rectangle(5.0, 5.0, 6.0, 6.0));
    EXPECT_TRUE(miss.empty());
}

TEST(FeatureSpatialIndexTest, PointFeatureZeroAreaBounds) {
    FeatureSpatialIndex idx;
    // 点要素 bounds 退化(west==east, south==north)。
    idx.insert(7, Rectangle(1.0, 1.0, 1.0, 1.0));
    auto hit = idx.query(Rectangle(0.0, 0.0, 2.0, 2.0));
    ASSERT_EQ(1u, hit.size());
    EXPECT_EQ(7u, hit[0]);
}

TEST(FeatureSpatialIndexTest, QueryCoveringAllReturnsAll) {
    FeatureSpatialIndex idx;
    for (FeatureId i = 1; i <= 50; ++i) {
        double x = static_cast<double>(i) * 0.01;
        idx.insert(i, Rectangle(x, x, x + 0.005, x + 0.005));
    }
    auto all = idx.query(Rectangle(-10, -10, 10, 10));
    EXPECT_EQ(50u, all.size());
    EXPECT_EQ(50u, toSet(all).size());  // 无重复
}

// ============================================================
// 分裂(超过 M 触发)
// ============================================================

TEST(FeatureSpatialIndexTest, ManyInsertsForceSplitsStillCorrect) {
    FeatureSpatialIndex idx;
    std::vector<std::pair<FeatureId, Rectangle>> items;
    // 200 条,远超 M=16,强制多层分裂。
    for (FeatureId i = 1; i <= 200; ++i) {
        double x = static_cast<double>(i) * 0.01;
        Rectangle r(x, 0.0, x + 0.02, 0.5);
        idx.insert(i, r);
        items.emplace_back(i, r);
    }
    EXPECT_EQ(200u, idx.size());

    // 若干确定性查询窗对拍。
    for (double qx = -0.1; qx < 2.2; qx += 0.13) {
        Rectangle q(qx, 0.0, qx + 0.1, 0.5);
        EXPECT_EQ(bruteForce(items, q), toSet(idx.query(q))) << "qx=" << qx;
    }
}

// ============================================================
// 删除 + condense
// ============================================================

TEST(FeatureSpatialIndexTest, RemoveThenQueryGone) {
    FeatureSpatialIndex idx;
    idx.insert(1, Rectangle(0, 0, 1, 1));
    idx.insert(2, Rectangle(0, 0, 1, 1));  // 同 bounds 不同 id
    EXPECT_EQ(2u, idx.size());

    EXPECT_TRUE(idx.remove(1, Rectangle(0, 0, 1, 1)));
    EXPECT_EQ(1u, idx.size());

    auto r = idx.query(Rectangle(0, 0, 1, 1));
    ASSERT_EQ(1u, r.size());
    EXPECT_EQ(2u, r[0]);  // 只剩 id=2

    EXPECT_FALSE(idx.remove(999, Rectangle(0, 0, 1, 1)));  // 不存在
    EXPECT_EQ(1u, idx.size());
}

TEST(FeatureSpatialIndexTest, RemoveManyTriggersCondenseStillCorrect) {
    FeatureSpatialIndex idx;
    std::vector<std::pair<FeatureId, Rectangle>> items;
    for (FeatureId i = 1; i <= 300; ++i) {
        double x = static_cast<double>(i) * 0.01;
        Rectangle r(x, 0.0, x + 0.02, 0.5);
        idx.insert(i, r);
        items.emplace_back(i, r);
    }
    // 删掉一半(奇数 id),触发大量 condense/重插。
    for (FeatureId i = 1; i <= 300; i += 2) {
        ASSERT_TRUE(idx.remove(i, Rectangle(static_cast<double>(i) * 0.01, 0.0,
                                            static_cast<double>(i) * 0.01 + 0.02, 0.5)));
    }
    EXPECT_EQ(150u, idx.size());

    // 剩余应只含偶数 id。
    std::vector<std::pair<FeatureId, Rectangle>> remaining;
    for (const auto& it : items)
        if (it.first % 2 == 0) remaining.push_back(it);

    for (double qx = -0.1; qx < 3.2; qx += 0.17) {
        Rectangle q(qx, 0.0, qx + 0.15, 0.5);
        EXPECT_EQ(bruteForce(remaining, q), toSet(idx.query(q))) << "qx=" << qx;
    }
}

// ============================================================
// 大规模随机对拍(P0 核心达成判据)
// ============================================================

TEST(FeatureSpatialIndexTest, LargeScaleRandomizedCrossCheck) {
    std::mt19937 rng(12345);  // 固定种子,确定性
    std::uniform_real_distribution<double> lng(-3.14159, 3.14159);
    std::uniform_real_distribution<double> lat(-1.5707, 1.5707);
    std::uniform_real_distribution<double> span(0.0001, 0.05);

    FeatureSpatialIndex idx;
    std::vector<std::pair<FeatureId, Rectangle>> items;
    constexpr int kN = 10000;
    for (FeatureId i = 1; i <= kN; ++i) {
        double w = lng(rng), s = lat(rng);
        Rectangle r(w, s, w + span(rng), s + span(rng));  // 非跨反经线小矩形
        idx.insert(i, r);
        items.emplace_back(i, r);
    }
    ASSERT_EQ(static_cast<size_t>(kN), idx.size());

    // 200 个随机查询窗,逐一对拍暴力扫描。
    for (int k = 0; k < 200; ++k) {
        double w = lng(rng), s = lat(rng);
        double qw = span(rng) * 10.0;  // 更大的查询窗
        Rectangle q(w, s, w + qw, s + qw);
        EXPECT_EQ(bruteForce(items, q), toSet(idx.query(q)))
            << "query #" << k;
    }
}

// ============================================================
// clear
// ============================================================

TEST(FeatureSpatialIndexTest, ClearResets) {
    FeatureSpatialIndex idx;
    for (FeatureId i = 1; i <= 30; ++i)
        idx.insert(i, Rectangle(i * 0.01, 0, i * 0.01 + 0.01, 0.1));
    idx.clear();
    EXPECT_TRUE(idx.empty());
    EXPECT_EQ(0u, idx.size());
    EXPECT_TRUE(idx.query(Rectangle(-10, -10, 10, 10)).empty());
}
