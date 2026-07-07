#include <gtest/gtest.h>

#include "earth_engine/data/ConstrainedDelaunay.h"

#include <cmath>
#include <random>
#include <set>
#include <vector>

using namespace earth_engine;
using Vec2 = glm::dvec2;
using Edge = ConstrainedDelaunay::Edge;

namespace {

double triArea(const Vec2& a, const Vec2& b, const Vec2& c) {
    return 0.5 * std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
}

double shoelace(const std::vector<Vec2>& poly) {
    double s = 0.0;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vec2& p = poly[i];
        const Vec2& q = poly[(i + 1) % poly.size()];
        s += p.x * q.y - q.x * p.y;
    }
    return 0.5 * s;  // 有符号
}

// 环上连续对构成的闭合约束边。offset = 该环起始点索引。
std::vector<Edge> ringEdges(uint32_t offset, uint32_t count) {
    std::vector<Edge> e;
    for (uint32_t i = 0; i < count; ++i)
        e.emplace_back(offset + i, offset + (i + 1) % count);
    return e;
}

double sumTriArea(const std::vector<Vec2>& pts,
                  const std::vector<uint32_t>& tris) {
    double a = 0.0;
    for (size_t i = 0; i < tris.size(); i += 3)
        a += triArea(pts[tris[i]], pts[tris[i + 1]], pts[tris[i + 2]]);
    return a;
}

bool hasEdge(const std::vector<uint32_t>& tris, uint32_t a, uint32_t b) {
    for (size_t i = 0; i < tris.size(); i += 3) {
        std::set<uint32_t> t{tris[i], tris[i + 1], tris[i + 2]};
        if (t.count(a) && t.count(b)) return true;
    }
    return false;
}

// 点是否在多边形内(even-odd)。
bool pointInPoly(const Vec2& p, const std::vector<Vec2>& poly) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Vec2& a = poly[i];
        const Vec2& b = poly[j];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
            in = !in;
        }
    }
    return in;
}

} // namespace

// ============================================================
// 简单多边形:三角形数 = n-2,面积守恒,约束边全在
// ============================================================

TEST(ConstrainedDelaunayTest, Square) {
    std::vector<Vec2> pts = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    auto tris = ConstrainedDelaunay::triangulate(pts, ringEdges(0, 4));
    EXPECT_EQ(6u, tris.size());  // 2 三角形
    EXPECT_NEAR(1.0, sumTriArea(pts, tris), 1e-12);
    for (const auto& e : ringEdges(0, 4))
        EXPECT_TRUE(hasEdge(tris, e.first, e.second));
}

TEST(ConstrainedDelaunayTest, ConvexPentagon) {
    std::vector<Vec2> pts = {{0, 0}, {2, 0}, {3, 2}, {1, 3}, {-1, 2}};
    auto tris = ConstrainedDelaunay::triangulate(pts, ringEdges(0, 5));
    EXPECT_EQ(9u, tris.size());  // 3 三角形
    EXPECT_NEAR(std::abs(shoelace(pts)), sumTriArea(pts, tris), 1e-9);
}

TEST(ConstrainedDelaunayTest, NonConvexLShape) {
    // L 形(6 顶点,凹)。
    std::vector<Vec2> pts = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    auto tris = ConstrainedDelaunay::triangulate(pts, ringEdges(0, 6));
    EXPECT_EQ(12u, tris.size());  // n-2 = 4 三角形
    EXPECT_NEAR(std::abs(shoelace(pts)), sumTriArea(pts, tris), 1e-9);
    for (const auto& e : ringEdges(0, 6))
        EXPECT_TRUE(hasEdge(tris, e.first, e.second));
    // 所有三角形质心在多边形内(凹处不越界)。
    for (size_t i = 0; i < tris.size(); i += 3) {
        Vec2 c = (pts[tris[i]] + pts[tris[i + 1]] + pts[tris[i + 2]]) / 3.0;
        EXPECT_TRUE(pointInPoly(c, pts)) << "tri " << i / 3;
    }
}

TEST(ConstrainedDelaunayTest, ConcaveStar) {
    // 五角星(10 顶点)。
    std::vector<Vec2> pts;
    for (int i = 0; i < 10; ++i) {
        double r = (i % 2 == 0) ? 1.0 : 0.4;
        double ang = M_PI / 2 + i * M_PI / 5;
        pts.emplace_back(r * std::cos(ang), r * std::sin(ang));
    }
    auto tris = ConstrainedDelaunay::triangulate(pts, ringEdges(0, 10));
    EXPECT_EQ(24u, tris.size());  // 8 三角形
    EXPECT_NEAR(std::abs(shoelace(pts)), sumTriArea(pts, tris), 1e-9);
    for (size_t i = 0; i < tris.size(); i += 3) {
        Vec2 c = (pts[tris[i]] + pts[tris[i + 1]] + pts[tris[i + 2]]) / 3.0;
        EXPECT_TRUE(pointInPoly(c, pts));
    }
}

// ============================================================
// 带孔:面积 = 外 - 孔,孔内无三角形
// ============================================================

TEST(ConstrainedDelaunayTest, SquareWithSquareHole) {
    // 外环 CCW(0..3),孔 CW(4..7)。
    std::vector<Vec2> pts = {
        {0, 0}, {4, 0}, {4, 4}, {0, 4},           // 外
        {1, 1}, {1, 3}, {3, 3}, {3, 1}};          // 孔(CW)
    std::vector<Edge> cons = ringEdges(0, 4);
    for (const auto& e : ringEdges(4, 4)) cons.push_back(e);

    auto tris = ConstrainedDelaunay::triangulate(pts, cons);
    // T = n + 2h - 2 = 8 + 2 - 2 = 8 三角形。
    EXPECT_EQ(24u, tris.size());
    EXPECT_NEAR(16.0 - 4.0, sumTriArea(pts, tris), 1e-9);  // 外 16 - 孔 4

    // 无三角形质心落在孔内。
    std::vector<Vec2> hole = {pts[4], pts[5], pts[6], pts[7]};
    for (size_t i = 0; i < tris.size(); i += 3) {
        Vec2 c = (pts[tris[i]] + pts[tris[i + 1]] + pts[tris[i + 2]]) / 3.0;
        EXPECT_FALSE(pointInPoly(c, hole)) << "tri " << i / 3 << " in hole";
    }
}

// ============================================================
// 退化:边上共线点
// ============================================================

TEST(ConstrainedDelaunayTest, CollinearPointOnEdge) {
    // 正方形,上边中点插入一个共线点(5 顶点)。
    std::vector<Vec2> pts = {{0, 0}, {2, 0}, {2, 2}, {1, 2}, {0, 2}};
    auto tris = ConstrainedDelaunay::triangulate(pts, ringEdges(0, 5));
    EXPECT_NEAR(4.0, sumTriArea(pts, tris), 1e-9);
    // 共线点 3 必被用到(约束边 2-3,3-4 强制)。
    EXPECT_TRUE(hasEdge(tris, 2, 3));
    EXPECT_TRUE(hasEdge(tris, 3, 4));
}

// ============================================================
// 边界:点数不足
// ============================================================

TEST(ConstrainedDelaunayTest, TooFewPoints) {
    std::vector<Vec2> pts = {{0, 0}, {1, 0}};
    EXPECT_TRUE(ConstrainedDelaunay::triangulate(pts, {}).empty());
}

// ============================================================
// 随机凸多边形对拍(面积守恒 + 约束边全在)
// ============================================================

TEST(ConstrainedDelaunayTest, RandomConvexPolygons) {
    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> nDist(4, 40);
    std::uniform_real_distribution<double> rDist(0.5, 2.0);

    for (int iter = 0; iter < 100; ++iter) {
        const int n = nDist(rng);
        // 角度递增生成凸多边形(半径随机)。
        std::vector<Vec2> pts;
        for (int i = 0; i < n; ++i) {
            double ang = 2.0 * M_PI * i / n;
            double r = rDist(rng);
            pts.emplace_back(r * std::cos(ang), r * std::sin(ang));
        }
        auto tris = ConstrainedDelaunay::triangulate(
            pts, ringEdges(0, static_cast<uint32_t>(n)));
        ASSERT_FALSE(tris.empty()) << "iter " << iter;
        EXPECT_EQ(static_cast<size_t>(3 * (n - 2)), tris.size()) << "iter " << iter;
        EXPECT_NEAR(std::abs(shoelace(pts)), sumTriArea(pts, tris), 1e-9)
            << "iter " << iter;
        for (const auto& e : ringEdges(0, static_cast<uint32_t>(n)))
            EXPECT_TRUE(hasEdge(tris, e.first, e.second)) << "iter " << iter;
    }
}
