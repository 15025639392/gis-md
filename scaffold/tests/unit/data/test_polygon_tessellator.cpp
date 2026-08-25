#include <gtest/gtest.h>

#include "earth_engine/data/PolygonTessellator.h"
#include "earth_engine/data/Feature.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/math/Vec3.h"

#include <algorithm>
#include <cmath>
#include <optional>

using namespace earth_engine;

namespace {

Feature polygon(std::vector<std::vector<Cartographic>> rings) {
    Feature f;
    f.type = GeometryType::Polygon;
    f.rings = std::move(rings);
    return f;
}

// 小方块环(radian),CCW。
std::vector<Cartographic> square(double w, double s, double e, double n,
                                 bool close) {
    std::vector<Cartographic> r = {Cartographic(w, s), Cartographic(e, s),
                                   Cartographic(e, n), Cartographic(w, n)};
    if (close) r.push_back(Cartographic(w, s));  // 显式闭合
    return r;
}

} // namespace

TEST(PolygonTessellatorTest, SimpleSquare) {
    Feature f = polygon({square(0.10, 0.10, 0.11, 0.11, false)});
    auto fill = PolygonTessellator::tessellate(f, Ellipsoid::WGS84());

    EXPECT_EQ(4u, fill.positions.size());     // 4 唯一顶点
    EXPECT_EQ(6u, fill.fillIndices.size());   // 2 三角形
    EXPECT_EQ(8u, fill.outlineIndices.size()); // 4 条边

    // 顶点在 WGS84 面附近(半径量级 6.36e6~6.38e6 m)。
    for (const auto& p : fill.positions) {
        double r = std::sqrt(p.x() * p.x() + p.y() * p.y() + p.z() * p.z());
        EXPECT_GT(r, 6.35e6);
        EXPECT_LT(r, 6.40e6);
    }
    // 索引不越界。
    for (uint32_t i : fill.fillIndices) EXPECT_LT(i, fill.positions.size());
}

TEST(PolygonTessellatorTest, ClosingVertexDeduped) {
    // 显式闭合(5 点,末==首)应去重成 4 唯一顶点。
    Feature f = polygon({square(0.2, 0.2, 0.21, 0.21, true)});
    auto fill = PolygonTessellator::tessellate(f, Ellipsoid::WGS84());
    EXPECT_EQ(4u, fill.positions.size());
    EXPECT_EQ(6u, fill.fillIndices.size());
}

TEST(PolygonTessellatorTest, PolygonWithHole) {
    // 外环 CCW + 孔 CW。
    std::vector<Cartographic> outer = {
        Cartographic(0.0, 0.0), Cartographic(0.4, 0.0),
        Cartographic(0.4, 0.4), Cartographic(0.0, 0.4)};
    std::vector<Cartographic> hole = {
        Cartographic(0.1, 0.1), Cartographic(0.1, 0.3),
        Cartographic(0.3, 0.3), Cartographic(0.3, 0.1)};
    Feature f = polygon({outer, hole});
    auto fill = PolygonTessellator::tessellate(f, Ellipsoid::WGS84());

    EXPECT_EQ(8u, fill.positions.size());       // 8 唯一顶点
    EXPECT_EQ(24u, fill.fillIndices.size());    // n+2h-2 = 8 三角形
    EXPECT_EQ(16u, fill.outlineIndices.size()); // 外 4 + 孔 4 = 8 边
}

TEST(PolygonTessellatorTest, NonPolygonReturnsEmpty) {
    Feature pt;
    pt.type = GeometryType::Point;
    pt.rings = {{Cartographic(0.1, 0.1)}};
    auto fill = PolygonTessellator::tessellate(pt, Ellipsoid::WGS84());
    EXPECT_TRUE(fill.positions.empty());
    EXPECT_TRUE(fill.fillIndices.empty());
}

TEST(PolygonTessellatorTest, DegenerateReturnsEmpty) {
    Feature f = polygon({{Cartographic(0.1, 0.1), Cartographic(0.2, 0.1)}});  // 2 点
    auto fill = PolygonTessellator::tessellate(f, Ellipsoid::WGS84());
    EXPECT_TRUE(fill.fillIndices.empty());
}

// ============================================================
// 自交/接触多边形(编辑畸形输入,even-odd 语义)
// ============================================================

namespace {

// fill 三角形的 3D 总面积(m²,叉积法;小尺度下椭球面≈平面)。
double fillArea3D(const TessellatedFill& fill) {
    double total = 0.0;
    for (size_t i = 0; i + 2 < fill.fillIndices.size(); i += 3) {
        const Vec3& a = fill.positions[fill.fillIndices[i]];
        const Vec3& b = fill.positions[fill.fillIndices[i + 1]];
        const Vec3& c = fill.positions[fill.fillIndices[i + 2]];
        const Vec3 ab = b - a;
        const Vec3 ac = c - a;
        const double cx = ab.y() * ac.z() - ab.z() * ac.y();
        const double cy = ab.z() * ac.x() - ab.x() * ac.z();
        const double cz = ab.x() * ac.y() - ab.y() * ac.x();
        total += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
    }
    return total;
}

} // namespace

TEST(PolygonTessellatorTest, SelfIntersectingBowtieEvenOddFill) {
    // 编辑把顶点拖过对边 → 蝴蝶结(自交)。even-odd 语义 = 两叶都填,
    // 总面积 = 同尺寸简单方形的一半。
    Feature simple = polygon({square(0.10, 0.10, 0.11, 0.11, false)});
    const double squareArea =
        fillArea3D(PolygonTessellator::tessellate(simple, Ellipsoid::WGS84()));
    ASSERT_GT(squareArea, 0.0);

    // 交换后两点 → (w,s),(e,s),(w,n),(e,n):边2、边4 在中心交叉。
    Feature bowtie = polygon({{Cartographic(0.10, 0.10),
                               Cartographic(0.11, 0.10),
                               Cartographic(0.10, 0.11),
                               Cartographic(0.11, 0.11)}});
    auto fill = PolygonTessellator::tessellate(bowtie, Ellipsoid::WGS84());
    ASSERT_FALSE(fill.fillIndices.empty());
    for (uint32_t idx : fill.fillIndices) {
        ASSERT_LT(idx, fill.positions.size());
    }
    // 交点应作为 Steiner 点被插入(4 原顶点 + ≥1 交点)。
    EXPECT_GE(fill.positions.size(), 5u);
    const double bowtieArea = fillArea3D(fill);
    EXPECT_NEAR(squareArea * 0.5, bowtieArea, squareArea * 0.02);
}

TEST(PolygonTessellatorTest, VertexTouchingEdgeStaysSane) {
    // 顶点被拖到恰好落在另一条边上(T 型接触):不崩、fill 非空、索引合法。
    Feature f = polygon({{Cartographic(0.10, 0.10),
                          Cartographic(0.105, 0.10),   // 恰在底边所在线上被引用
                          Cartographic(0.11, 0.10),
                          Cartographic(0.11, 0.11),
                          Cartographic(0.105, 0.10),   // 拖到底边上的顶点(重复引用)
                          Cartographic(0.10, 0.11)}});
    auto fill = PolygonTessellator::tessellate(f, Ellipsoid::WGS84());
    ASSERT_FALSE(fill.fillIndices.empty());
    for (uint32_t idx : fill.fillIndices) {
        ASSERT_LT(idx, fill.positions.size());
    }
    EXPECT_GT(fillArea3D(fill), 0.0);
}

namespace {

double maxTriangleEdgeMeters(const TessellatedFill& fill) {
    double m = 0.0;
    for (size_t i = 0; i + 2 < fill.fillIndices.size(); i += 3) {
        const Vec3& a = fill.positions[fill.fillIndices[i]];
        const Vec3& b = fill.positions[fill.fillIndices[i + 1]];
        const Vec3& c = fill.positions[fill.fillIndices[i + 2]];
        m = std::max(m, (b - a).length());
        m = std::max(m, (c - b).length());
        m = std::max(m, (a - c).length());
    }
    return m;
}

double maxChordSagMeters(const TessellatedFill& fill, const Ellipsoid& e) {
    double sag = 0.0;
    auto consider = [&](const Vec3& a, const Vec3& b) {
        const Vec3 mid = (a + b) * 0.5;
        const std::optional<Vec3> surf = e.tryScaleToGeodeticSurface(mid);
        if (!surf) return;
        sag = std::max(sag, (*surf - mid).length());
    };
    for (size_t i = 0; i + 2 < fill.fillIndices.size(); i += 3) {
        const Vec3& a = fill.positions[fill.fillIndices[i]];
        const Vec3& b = fill.positions[fill.fillIndices[i + 1]];
        const Vec3& c = fill.positions[fill.fillIndices[i + 2]];
        consider(a, b);
        consider(b, c);
        consider(c, a);
    }
    return sag;
}

}  // namespace

TEST(PolygonTessellatorTest, GlobeSubdivKeepsTriangleEdgesShort) {
    // ~1.15° 方形(赤道约 127km)。不细分时对角线跨过整面;细分后边长
    // 落在 maxEdge 的 √2 邻域(网格斜边)。
    Feature f = polygon({square(0.0, 0.0, 0.02, 0.02, false)});
    const auto coarse =
        PolygonTessellator::tessellate(f, Ellipsoid::WGS84(), 0.0, nullptr, 0.0);
    const auto fine = PolygonTessellator::tessellate(
        f, Ellipsoid::WGS84(), 0.0, nullptr, 10000.0);
    ASSERT_FALSE(coarse.fillIndices.empty());
    ASSERT_FALSE(fine.fillIndices.empty());
    EXPECT_GT(maxTriangleEdgeMeters(coarse), 50000.0);
    EXPECT_LT(maxTriangleEdgeMeters(fine), 10000.0 * 1.8);
    EXPECT_GT(fine.positions.size(), coarse.positions.size());
}

TEST(PolygonTessellatorTest, GlobeSubdivReducesChordSag) {
    Feature f = polygon({square(0.0, 0.0, 0.02, 0.02, false)});
    const Ellipsoid& e = Ellipsoid::WGS84();
    const auto coarse = PolygonTessellator::tessellate(f, e, 0.0, nullptr, 0.0);
    const auto fine = PolygonTessellator::tessellate(f, e, 0.0, nullptr, 10000.0);
    EXPECT_GT(maxChordSagMeters(coarse, e), 50.0);
    EXPECT_LT(maxChordSagMeters(fine, e), 15.0);
}

TEST(PolygonTessellatorTest, DefaultMaxEdgeDoesNotChangeSmallSquare) {
    Feature f = polygon({square(0.10, 0.10, 0.11, 0.11, false)});
    auto fill = PolygonTessellator::tessellate(f, Ellipsoid::WGS84());
    EXPECT_EQ(4u, fill.positions.size());
    EXPECT_EQ(6u, fill.fillIndices.size());
}

TEST(PolygonTessellatorTest, SubMaxEdgePolygonSkipsGlobeGrid) {
    // ~255m 方形 < 400m:边不拆、不撒 Steiner,保持 2 三角。
    constexpr double kSpan = 4.0e-5;
    Feature f = polygon({square(0.10, 0.10, 0.10 + kSpan, 0.10 + kSpan, false)});
    auto fill = PolygonTessellator::tessellate(
        f, Ellipsoid::WGS84(), 0.0, nullptr, 400.0);
    EXPECT_EQ(4u, fill.positions.size());
    EXPECT_EQ(6u, fill.fillIndices.size());
}
