#include <gtest/gtest.h>

#include "earth_engine/data/PolygonTessellator.h"
#include "earth_engine/data/Feature.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"

#include <cmath>

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
