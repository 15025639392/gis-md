#include <gtest/gtest.h>
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

TEST(Vec3Test, DefaultConstructor) {
    Vec3 v;
    EXPECT_DOUBLE_EQ(0.0, v.x());
    EXPECT_DOUBLE_EQ(0.0, v.y());
    EXPECT_DOUBLE_EQ(0.0, v.z());
}

TEST(Vec3Test, ParameterizedConstructor) {
    Vec3 v(1.0, 2.0, 3.0);
    EXPECT_DOUBLE_EQ(1.0, v.x());
    EXPECT_DOUBLE_EQ(2.0, v.y());
    EXPECT_DOUBLE_EQ(3.0, v.z());
}

TEST(Vec3Test, Addition) {
    Vec3 a(1, 2, 3);
    Vec3 b(4, 5, 6);
    Vec3 c = a + b;
    EXPECT_DOUBLE_EQ(5, c.x());
    EXPECT_DOUBLE_EQ(7, c.y());
    EXPECT_DOUBLE_EQ(9, c.z());
}

TEST(Vec3Test, Subtraction) {
    Vec3 a(4, 5, 6);
    Vec3 b(1, 2, 3);
    Vec3 c = a - b;
    EXPECT_DOUBLE_EQ(3, c.x());
    EXPECT_DOUBLE_EQ(3, c.y());
    EXPECT_DOUBLE_EQ(3, c.z());
}

TEST(Vec3Test, ScalarMultiplication) {
    Vec3 v(1, 2, 3);
    Vec3 r = v * 2.0;
    EXPECT_DOUBLE_EQ(2, r.x());
    EXPECT_DOUBLE_EQ(4, r.y());
    EXPECT_DOUBLE_EQ(6, r.z());

    r = 3.0 * v;
    EXPECT_DOUBLE_EQ(3, r.x());
    EXPECT_DOUBLE_EQ(6, r.y());
    EXPECT_DOUBLE_EQ(9, r.z());
}

TEST(Vec3Test, DotProduct) {
    Vec3 a(1, 0, 0);
    Vec3 b(0, 1, 0);
    EXPECT_DOUBLE_EQ(0.0, a.dot(b));

    Vec3 c(2, 3, 4);
    EXPECT_DOUBLE_EQ(2*2+3*3+4*4, c.dot(c));
}

TEST(Vec3Test, CrossProduct) {
    Vec3 x = Vec3::unitX();
    Vec3 y = Vec3::unitY();
    Vec3 z = x.cross(y);
    EXPECT_NEAR(0, z.x(), 1e-12);
    EXPECT_NEAR(0, z.y(), 1e-12);
    EXPECT_NEAR(1, z.z(), 1e-12);
}

TEST(Vec3Test, Length) {
    Vec3 v(3, 4, 0);
    EXPECT_DOUBLE_EQ(5.0, v.length());
}

TEST(Vec3Test, Normalized) {
    Vec3 v(3, 4, 0);
    Vec3 n = v.normalized();
    EXPECT_NEAR(0.6, n.x(), 1e-12);
    EXPECT_NEAR(0.8, n.y(), 1e-12);
    EXPECT_NEAR(0.0, n.z(), 1e-12);
    EXPECT_NEAR(1.0, n.length(), 1e-12);
}

TEST(Vec3Test, Distance) {
    Vec3 a(0, 0, 0);
    Vec3 b(3, 4, 0);
    EXPECT_DOUBLE_EQ(5.0, a.distanceTo(b));
}

TEST(Vec3Test, Negation) {
    Vec3 v(1, -2, 3);
    Vec3 n = -v;
    EXPECT_DOUBLE_EQ(-1, n.x());
    EXPECT_DOUBLE_EQ(2, n.y());
    EXPECT_DOUBLE_EQ(-3, n.z());
}

TEST(Vec3Test, GlmInterop) {
    Vec3 v(1, 2, 3);
    glm::dvec3 raw = v.raw();
    EXPECT_DOUBLE_EQ(1, raw.x);
    EXPECT_DOUBLE_EQ(2, raw.y);
    EXPECT_DOUBLE_EQ(3, raw.z);

    Vec3 fromGlm(raw);
    EXPECT_EQ(v, fromGlm);
}
