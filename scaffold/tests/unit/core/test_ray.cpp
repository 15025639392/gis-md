#include <gtest/gtest.h>

#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Ray.h"
#include "earth_engine/core/math/Vec3.h"

#include <stdexcept>
#include <type_traits>

using namespace earth_engine;

namespace {

void expectVec3Near(const Vec3& actual, const Vec3& expected, double epsilon) {
    EXPECT_NEAR(expected.x(), actual.x(), epsilon);
    EXPECT_NEAR(expected.y(), actual.y(), epsilon);
    EXPECT_NEAR(expected.z(), actual.z(), epsilon);
}

} // namespace

static_assert(!std::is_default_constructible_v<Ray>,
              "Ray matches cesium-native: origin and normalized direction are required.");

TEST(RayTest, ConstructorRequiresNormalizedDirection) {
    // Ported from cesium-native CesiumGeometry::Ray constructor semantics:
    // direction is an input contract, not silently normalized by Ray.
    EXPECT_NO_THROW(Ray(Vec3(1.0, 2.0, 3.0), Vec3::unitX()));
    EXPECT_NO_THROW(Ray(Vec3(1.0, 2.0, 3.0), Vec3(1.0 + 1e-6, 0.0, 0.0)));
    EXPECT_THROW(Ray(Vec3(1.0, 2.0, 3.0), Vec3(2.0, 0.0, 0.0)),
                 std::invalid_argument);
    EXPECT_THROW(Ray(Vec3(1.0, 2.0, 3.0), Vec3(1.0 + 2e-6, 0.0, 0.0)),
                 std::invalid_argument);
}

TEST(RayTest, PointAtMatchesCesiumNativePointFromDistance) {
    const Ray ray(Vec3(1.0, 2.0, 3.0), Vec3::unitY());

    EXPECT_EQ(Vec3(1.0, 7.0, 3.0), ray.pointAt(5.0));
    EXPECT_EQ(Vec3(1.0, -1.0, 3.0), ray.pointAt(-3.0));
}

TEST(RayTest, TransformAppliesPointAndDirectionSemantics) {
    const Ray ray(Vec3(1.0, 2.0, 3.0), Vec3::unitX());
    const Mat4 transform = Mat4::translation(Vec3(10.0, 20.0, 30.0)) *
                           Mat4::rotationZ(1.57079632679489661923) *
                           Mat4::scale(Vec3(2.0, 3.0, 4.0));

    const Ray transformed = ray.transform(transform);

    expectVec3Near(transformed.origin(), Vec3(4.0, 22.0, 42.0), 1e-12);
    expectVec3Near(transformed.direction(), Vec3::unitY(), 1e-12);
    EXPECT_NEAR(1.0, transformed.direction().length(), 1e-12);
}

TEST(RayTest, UnaryMinusReversesDirectionOnly) {
    const Ray ray(Vec3(1.0, 2.0, 3.0), Vec3::unitZ());

    const Ray reversed = -ray;

    EXPECT_EQ(ray.origin(), reversed.origin());
    EXPECT_EQ(Vec3(0.0, 0.0, -1.0), reversed.direction());
}
