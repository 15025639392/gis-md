#include <gtest/gtest.h>

#include "earth_engine/data/LineTessellator.h"
#include "earth_engine/data/Feature.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"

#include <cmath>

using namespace earth_engine;

namespace {

Feature line(std::vector<Cartographic> pts) {
    Feature f;
    f.type = GeometryType::LineString;
    f.rings = {std::move(pts)};
    return f;
}

double dist3(const Vec3& a, const Vec3& b) {
    const double dx = a.x() - b.x(), dy = a.y() - b.y(), dz = a.z() - b.z();
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool sameVec(const Vec3& a, const Vec3& b) { return dist3(a, b) == 0.0; }

} // namespace

TEST(LineTessellatorTest, OpenTwoPoints) {
    Feature f = line({Cartographic(0.10, 0.10), Cartographic(0.12, 0.10)});
    auto t = LineTessellator::tessellate(f, Ellipsoid::WGS84());

    EXPECT_EQ(4u, t.vertices.size());   // 2n
    EXPECT_EQ(6u, t.indices.size());    // 1 段 × 2 三角形

    // side 交替 +1/-1。
    EXPECT_FLOAT_EQ(1.0f, t.vertices[0].side);
    EXPECT_FLOAT_EQ(-1.0f, t.vertices[1].side);

    // 端点哨兵:顶点0 prev==pos;顶点1(索引2/3) next==pos。
    EXPECT_TRUE(sameVec(t.vertices[0].prev, t.vertices[0].pos));
    EXPECT_TRUE(sameVec(t.vertices[2].next, t.vertices[2].pos));
    // 顶点0 的 next == 顶点1 的 pos(相邻)。
    EXPECT_TRUE(sameVec(t.vertices[0].next, t.vertices[2].pos));

    // lengthSoFar:起点 0,终点 = 两点 ECEF 距离。
    EXPECT_FLOAT_EQ(0.0f, t.vertices[0].lengthSoFar);
    EXPECT_GT(t.vertices[2].lengthSoFar, 0.0f);
    EXPECT_FLOAT_EQ(t.vertices[2].lengthSoFar,
                    static_cast<float>(dist3(t.vertices[0].pos, t.vertices[2].pos)));

    // 索引不越界。
    for (uint32_t i : t.indices) EXPECT_LT(i, t.vertices.size());
}

TEST(LineTessellatorTest, OpenThreePointsInteriorNeighbors) {
    Feature f = line({Cartographic(0.10, 0.10), Cartographic(0.11, 0.10),
                      Cartographic(0.12, 0.11)});
    auto t = LineTessellator::tessellate(f, Ellipsoid::WGS84());
    EXPECT_EQ(6u, t.vertices.size());
    EXPECT_EQ(12u, t.indices.size());  // 2 段

    // 中间顶点(索引2/3, 折线顶点1)prev/next 指向邻居,非哨兵。
    EXPECT_TRUE(sameVec(t.vertices[2].prev, t.vertices[0].pos));
    EXPECT_TRUE(sameVec(t.vertices[2].next, t.vertices[4].pos));
    // lengthSoFar 单调递增。
    EXPECT_LT(t.vertices[0].lengthSoFar, t.vertices[2].lengthSoFar);
    EXPECT_LT(t.vertices[2].lengthSoFar, t.vertices[4].lengthSoFar);
}

TEST(LineTessellatorTest, ConsecutiveDuplicatesDeduped) {
    Feature f = line({Cartographic(0.10, 0.10), Cartographic(0.10, 0.10),
                      Cartographic(0.12, 0.10)});
    auto t = LineTessellator::tessellate(f, Ellipsoid::WGS84());
    EXPECT_EQ(4u, t.vertices.size());  // 重复点去掉 → 2 顶点
}

TEST(LineTessellatorTest, ClosedWraps) {
    // 三角形环,closed=true。首尾不重合的三个点。
    Feature f = line({Cartographic(0.10, 0.10), Cartographic(0.12, 0.10),
                      Cartographic(0.11, 0.12)});
    auto t = LineTessellator::tessellate(f, Ellipsoid::WGS84(), 0.0, /*closed*/ true);
    EXPECT_EQ(6u, t.vertices.size());
    EXPECT_EQ(18u, t.indices.size());  // closed → n 段 = 3

    // 顶点0 prev 环绕到末点;末顶点 next 环绕到首点。
    EXPECT_TRUE(sameVec(t.vertices[0].prev, t.vertices[4].pos));  // 折线顶点2
    EXPECT_TRUE(sameVec(t.vertices[4].next, t.vertices[0].pos));
}

TEST(LineTessellatorTest, ClosedDropsCoincidentClosingPoint) {
    // 显式闭合(首尾重合)+ closed → 末点丢弃,3 顶点。
    Feature f = line({Cartographic(0.10, 0.10), Cartographic(0.12, 0.10),
                      Cartographic(0.11, 0.12), Cartographic(0.10, 0.10)});
    auto t = LineTessellator::tessellate(f, Ellipsoid::WGS84(), 0.0, true);
    EXPECT_EQ(6u, t.vertices.size());  // 4 输入 → 去闭合末点 → 3 顶点 → 6 ribbon
    EXPECT_EQ(18u, t.indices.size());
}

TEST(LineTessellatorTest, DegenerateAndWrongType) {
    Feature one = line({Cartographic(0.1, 0.1)});
    EXPECT_TRUE(LineTessellator::tessellate(one, Ellipsoid::WGS84()).vertices.empty());

    Feature poly;
    poly.type = GeometryType::Polygon;
    poly.rings = {{Cartographic(0, 0), Cartographic(1, 0), Cartographic(1, 1)}};
    EXPECT_TRUE(LineTessellator::tessellate(poly, Ellipsoid::WGS84()).vertices.empty());
}
