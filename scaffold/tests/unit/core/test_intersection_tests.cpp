#include <gtest/gtest.h>

#include "earth_engine/core/math/AxisAlignedBox.h"
#include "earth_engine/core/math/BoundingSphere.h"
#include "earth_engine/core/math/IntersectionTests.h"
#include "earth_engine/core/math/OrientedBoundingBox.h"
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Ray.h"
#include "earth_engine/core/math/Vec3.h"

#include <array>
#include <cmath>
#include <glm/ext/matrix_double3x3.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/vector_double2.hpp>
#include <optional>

using namespace earth_engine;

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;

double radians(double degrees) {
    return degrees * kPi / 180.0;
}

OrientedBoundingBox makeObb(const Vec3& center, const glm::dmat3& halfAxes) {
    return OrientedBoundingBox(center,
                               Vec3(halfAxes[0]),
                               Vec3(halfAxes[1]),
                               Vec3(halfAxes[2]));
}

glm::dmat3 rotation3(double angleRadians, const glm::dvec3& axis) {
    return glm::dmat3(glm::rotate(glm::dmat4(1.0), angleRadians, axis));
}

glm::dmat3 scale3(const glm::dvec3& scale) {
    return glm::dmat3(glm::scale(glm::dmat4(1.0), scale));
}

void expectVec3Near(const Vec3& expected, const Vec3& actual, double epsilon) {
    EXPECT_NEAR(expected.x(), actual.x(), epsilon);
    EXPECT_NEAR(expected.y(), actual.y(), epsilon);
    EXPECT_NEAR(expected.z(), actual.z(), epsilon);
}
} // namespace

TEST(IntersectionTestsTest, RayPlaneMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp.
    struct Case {
        Ray ray;
        Plane plane;
        std::optional<Vec3> expected;
    };

    const Case cases[] = {
        {
            Ray(Vec3(2.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
            Plane(Vec3(1.0, 0.0, 0.0), -1.0),
            Vec3(1.0, 0.0, 0.0)
        },
        {
            Ray(Vec3(2.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)),
            Plane(Vec3(1.0, 0.0, 0.0), -1.0),
            std::nullopt
        },
        {
            Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, 1.0, 0.0)),
            Plane(Vec3(1.0, 0.0, 0.0), -1.0),
            std::nullopt
        }
    };

    for (const Case& testCase : cases) {
        EXPECT_EQ(testCase.expected,
                  IntersectionTests::rayPlane(testCase.ray, testCase.plane));
    }
}

TEST(IntersectionTestsTest, RayPlaneTreatsNearParallelAsMiss) {
    const Ray ray(Vec3(0.0, 0.0, 0.0), Vec3::unitY());
    const Plane plane(Vec3::unitX(), -1.0);

    EXPECT_FALSE(IntersectionTests::rayPlane(ray, plane).has_value());
}

TEST(IntersectionTestsTest, RayPlaneReturnsOriginWhenRayStartsOnPlane) {
    // Source-derived from cesium-native IntersectionTests::rayPlane:
    // t == 0 is a valid intersection because only t < 0 is rejected.
    const Ray ray(Vec3(1.0, 2.0, 3.0), Vec3::unitZ());
    const Plane plane(Vec3::unitZ(), -3.0);

    EXPECT_EQ(ray.origin(), IntersectionTests::rayPlane(ray, plane));
}

TEST(IntersectionTestsTest, RayEllipsoidMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp.
    struct Case {
        Ray ray;
        Vec3 radii;
        std::optional<RayEllipsoidIntersectionInterval> expected;
    };

    const Vec3 unitRadii(1.0, 1.0, 1.0);
    const Vec3 wgs84Radii(6378137.0, 6378137.0, 6356752.3142451793);

    const Case cases[] = {
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
         Vec3::zero(),
         std::nullopt},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
         unitRadii,
         RayEllipsoidIntersectionInterval{1.0, 3.0}},
        {Ray(Vec3(0.0, 2.0, 0.0), Vec3(0.0, -1.0, 0.0)),
         unitRadii,
         RayEllipsoidIntersectionInterval{1.0, 3.0}},
        {Ray(Vec3(0.0, 0.0, 2.0), Vec3(0.0, 0.0, -1.0)),
         unitRadii,
         RayEllipsoidIntersectionInterval{1.0, 3.0}},
        {Ray(Vec3(-2.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)),
         unitRadii,
         RayEllipsoidIntersectionInterval{1.0, 3.0}},
        {Ray(Vec3(0.0, -2.0, 0.0), Vec3(0.0, 1.0, 0.0)),
         unitRadii,
         RayEllipsoidIntersectionInterval{1.0, 3.0}},
        {Ray(Vec3(0.0, 0.0, -2.0), Vec3(0.0, 0.0, 1.0)),
         unitRadii,
         RayEllipsoidIntersectionInterval{1.0, 3.0}},
        {Ray(Vec3(-2.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)), unitRadii, std::nullopt},
        {Ray(Vec3(0.0, -2.0, 0.0), Vec3(0.0, -1.0, 0.0)), unitRadii, std::nullopt},
        {Ray(Vec3(0.0, 0.0, -2.0), Vec3(0.0, 0.0, -1.0)), unitRadii, std::nullopt},
        {Ray(Vec3(20000.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)),
         wgs84Radii,
         RayEllipsoidIntersectionInterval{0.0, wgs84Radii.x() - 20000.0}},
        {Ray(Vec3(1.0, 0.0, 0.0), Vec3(0.0, 0.0, 1.0)), unitRadii, std::nullopt},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, 0.0, 1.0)), unitRadii, std::nullopt},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, 0.0, -1.0)), unitRadii, std::nullopt},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, 1.0, 0.0)), unitRadii, std::nullopt},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, -1.0, 0.0)), unitRadii, std::nullopt}
    };

    for (const Case& testCase : cases) {
        const auto actual = IntersectionTests::rayEllipsoid(testCase.ray, testCase.radii);
        ASSERT_EQ(testCase.expected.has_value(), actual.has_value());
        if (testCase.expected && actual) {
            EXPECT_NEAR(testCase.expected->entryDistance, actual->entryDistance, 1e-12);
            EXPECT_NEAR(testCase.expected->exitDistance, actual->exitDistance, 1e-6);
        }
    }
}

TEST(IntersectionTestsTest, RayEllipsoidReturnsRepeatedRootForOutsideTangent) {
    // Source-derived from cesium-native IntersectionTests::rayEllipsoid:
    // outside rays with qw2 == product return the repeated tangent distance
    // for both entry and exit.
    const auto interval = IntersectionTests::rayEllipsoid(
        Ray(Vec3(1.0, 1.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
        Vec3(1.0, 1.0, 1.0));

    ASSERT_TRUE(interval.has_value());
    EXPECT_DOUBLE_EQ(1.0, interval->entryDistance);
    EXPECT_DOUBLE_EQ(1.0, interval->exitDistance);
}

TEST(IntersectionTestsTest, RayTriangleMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp.
    const Vec3 v0(-1.0, 0.0, 0.0);
    const Vec3 v1(1.0, 0.0, 0.0);
    const Vec3 v2(0.0, 1.0, 0.0);

    struct Case {
        Ray ray;
        bool cullBackFaces;
        std::optional<Vec3> expected;
    };

    const Case cases[] = {
        {Ray(Vec3(0.0, 0.0, 1.0), Vec3(0.0, 0.0, -1.0)),
         false,
         Vec3(0.0, 0.0, 0.0)},
        {Ray(Vec3(0.0, 0.0, -1.0), Vec3(0.0, 0.0, 1.0)),
         false,
         Vec3(0.0, 0.0, 0.0)},
        {Ray(Vec3(0.0, 0.0, -1.0), Vec3(0.0, 0.0, 1.0)),
         true,
         std::nullopt},
        {Ray(Vec3(0.0, -1.0, 1.0), Vec3(0.0, 0.0, -1.0)),
         false,
         std::nullopt},
        {Ray(Vec3(1.0, 1.0, 10.0), Vec3(0.0, 0.0, -1.0)),
         false,
         std::nullopt},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, 1.0, 0.0)),
         false,
         std::nullopt},
        {Ray(Vec3(-1.0, 1.0, 1.0), Vec3(0.0, 0.0, -1.0)),
         false,
         std::nullopt},
        {Ray(Vec3(-1.0, 0.0, 1.0), Vec3(1.0, 0.0, 0.0)),
         false,
         std::nullopt},
        {Ray(Vec3(0.0, 0.0, 1.0), Vec3(0.0, 0.0, 1.0)),
         false,
         std::nullopt}
    };

    for (const Case& testCase : cases) {
        EXPECT_EQ(testCase.expected,
                  IntersectionTests::rayTriangle(testCase.ray,
                                                 v0,
                                                 v1,
                                                 v2,
                                                 testCase.cullBackFaces));
    }
}

TEST(IntersectionTestsTest, RayTriangleParametricReturnsDistanceAlongRay) {
    const Ray ray(Vec3(0.25, 0.25, 2.0), Vec3(0.0, 0.0, -1.0));

    const auto t = IntersectionTests::rayTriangleParametric(
        ray,
        Vec3(-1.0, 0.0, 0.0),
        Vec3(1.0, 0.0, 0.0),
        Vec3(0.0, 1.0, 0.0),
        false);

    ASSERT_TRUE(t.has_value());
    EXPECT_DOUBLE_EQ(2.0, *t);
}

TEST(IntersectionTestsTest, RayTriangleParametricPreservesNegativeDistance) {
    // Source-derived from cesium-native IntersectionTests::rayTriangle:
    // the parametric helper reports hits behind the origin, while the point
    // wrapper filters them out.
    const Ray ray(Vec3(0.25, 0.25, 1.0), Vec3(0.0, 0.0, 1.0));
    const Vec3 v0(-1.0, 0.0, 0.0);
    const Vec3 v1(1.0, 0.0, 0.0);
    const Vec3 v2(0.0, 1.0, 0.0);

    const auto t = IntersectionTests::rayTriangleParametric(
        ray,
        v0,
        v1,
        v2,
        false);

    ASSERT_TRUE(t.has_value());
    EXPECT_DOUBLE_EQ(-1.0, *t);
    EXPECT_FALSE(IntersectionTests::rayTriangle(ray, v0, v1, v2, false)
                     .has_value());
}

TEST(IntersectionTestsTest, RayAabbMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp.
    struct Case {
        Ray ray;
        AxisAlignedBox aabb;
        std::optional<Vec3> expected;
    };

    const double invSqrt2 = 1.0 / std::sqrt(2.0);
    const Case cases[] = {
        {
            Ray(Vec3(-1.0, 0.5, 0.5), Vec3(1.0, 0.0, 0.0)),
            AxisAlignedBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0),
            Vec3(0.0, 0.5, 0.5)
        },
        {
            Ray(Vec3(-1.0, 0.0, 1.0), Vec3(invSqrt2, 0.0, -invSqrt2)),
            AxisAlignedBox(-0.5, -0.5, -0.5, 0.5, 0.5, 0.5),
            Vec3(-0.5, 0.0, 0.5)
        },
        {
            Ray(Vec3(-1.0, 0.5, 0.5), Vec3(-1.0, 0.0, 0.0)),
            AxisAlignedBox(0.0, 0.0, 0.0, 1.0, 1.0, 1.0),
            std::nullopt
        },
        {
            Ray(Vec3(0.0, 0.0, 0.0), Vec3(0.0, -1.0, 0.0)),
            AxisAlignedBox(-1.0, -1.0, -1.0, 1.0, 1.0, 1.0),
            Vec3(0.0, -1.0, 0.0)
        }
    };

    for (const Case& testCase : cases) {
        EXPECT_EQ(testCase.expected,
                  IntersectionTests::rayAABB(testCase.ray, testCase.aabb));
    }
}

TEST(IntersectionTestsTest, RayAabbParametricReturnsEntryOrExitDistance) {
    const AxisAlignedBox aabb(-1.0, -1.0, -1.0, 1.0, 1.0, 1.0);

    const auto outside = IntersectionTests::rayAABBParametric(
        Ray(Vec3(-3.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)),
        aabb);
    ASSERT_TRUE(outside.has_value());
    EXPECT_DOUBLE_EQ(2.0, *outside);

    const auto inside = IntersectionTests::rayAABBParametric(
        Ray(Vec3(0.0, 0.0, 0.0), Vec3(0.0, 1.0, 0.0)),
        aabb);
    ASSERT_TRUE(inside.has_value());
    EXPECT_DOUBLE_EQ(1.0, *inside);
}

TEST(IntersectionTestsTest, RayAabbParametricSkipsNearParallelAxesLikeCesiumNative) {
    // Cesium-native skips axes with abs(direction) < Epsilon6 without checking
    // whether the origin lies within that slab.
    const AxisAlignedBox aabb(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
    const Ray ray(Vec3(-1.0, 2.0, 0.5), Vec3(1.0, 0.0, 0.0));

    const auto t = IntersectionTests::rayAABBParametric(ray, aabb);

    ASSERT_TRUE(t.has_value());
    EXPECT_DOUBLE_EQ(1.0, *t);
    EXPECT_EQ(Vec3(0.0, 2.0, 0.5), IntersectionTests::rayAABB(ray, aabb));
}

TEST(IntersectionTestsTest, RayObbMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp.
    struct Case {
        Ray ray;
        OrientedBoundingBox obb;
        Vec3 expected;
    };

    const double sqrt2 = std::sqrt(2.0);
    const double sqrt3 = std::sqrt(3.0);
    const double sqrt8 = std::sqrt(8.0);
    const glm::dvec3 xAxis(1.0, 0.0, 0.0);
    const glm::dvec3 yAxis(0.0, 1.0, 0.0);

    const Case cases[] = {
        {
            Ray(Vec3(0.0, 0.0, 10.0), Vec3(0.0, 0.0, -1.0)),
            makeObb(Vec3(0.0, 0.0, 0.0),
                    rotation3(radians(-45.0), xAxis)),
            Vec3(0.0, 0.0, sqrt2)
        },
        {
            Ray(Vec3(10.0, 10.0, 20.0), Vec3(0.0, 0.0, -1.0)),
            makeObb(Vec3(10.0, 10.0, 10.0),
                    rotation3(radians(-45.0), xAxis)),
            Vec3(10.0, 10.0, 10.0 + sqrt2)
        },
        {
            Ray(Vec3(10.0, 22.0, 31.0 + sqrt2),
                Vec3(0.0, -2.0, -1.0).normalized()),
            makeObb(Vec3(10.0, 20.0, 30.0),
                    rotation3(radians(-45.0), xAxis)),
            Vec3(10.0, 20.0, 30.0 + sqrt2)
        },
        {
            Ray(Vec3(10.0, 10.0, 20.0), Vec3(0.0, 0.0, -1.0)),
            makeObb(Vec3(10.0, 10.0, 10.0),
                    2.0 * rotation3(radians(-45.0), xAxis)),
            Vec3(10.0, 10.0, 10.0 + sqrt8)
        },
        {
            Ray(Vec3(10.0, 30.0, 50.0 + sqrt8),
                Vec3(0.0, -1.0, -2.0).normalized()),
            makeObb(Vec3(10.0, 20.0, 30.0),
                    2.0 * rotation3(radians(-45.0), xAxis)),
            Vec3(10.0, 20.0, 30.0 + sqrt8)
        },
        {
            Ray(Vec3(10.0, 10.0, 20.0), Vec3(0.0, 0.0, -1.0)),
            makeObb(Vec3(10.0, 10.0, 10.0),
                    scale3(glm::dvec3(2.0, 2.0, 1.0))),
            Vec3(10.0, 10.0, 11.0)
        },
        {
            Ray(Vec3(10.0, 20.0, 40.0), Vec3(0.0, 0.0, -1.0)),
            makeObb(Vec3(10.0, 20.0, 30.0),
                    scale3(glm::dvec3(2.0, 1.0, 2.0))),
            Vec3(10.0, 20.0, 32.0)
        },
        {
            Ray(Vec3(10.0, 20.0, 40.0), Vec3(0.0, 0.0, -1.0)),
            makeObb(Vec3(10.0, 20.0, 30.0),
                    scale3(glm::dvec3(1.0, 2.0, 1.0)) *
                        rotation3(radians(45.0), yAxis)),
            Vec3(10.0, 20.0, 30.0 + sqrt2)
        },
        {
            Ray(Vec3(10.0, 20.0, 40.0), Vec3(0.0, 0.0, -1.0)),
            makeObb(Vec3(10.0, 20.0, 30.0),
                    rotation3(radians(45.0), xAxis) *
                        scale3(glm::dvec3(1.0, 2.0, 1.0))),
            Vec3(10.0, 20.0, 30.0 + 1.0 / std::cos(radians(45.0)))
        },
        {
            Ray(Vec3(10.0, 20.0, 40.0), Vec3(0.0, 0.0, -1.0)),
            makeObb(Vec3(10.0, 20.0, 30.0),
                    scale3(glm::dvec3(1.0, 2.0, 1.0)) *
                        rotation3(radians(225.0), yAxis)),
            Vec3(10.0, 20.0, 30.0 + sqrt2)
        },
        {
            Ray(Vec3(10.0, 22.0, 32.0),
                Vec3(0.0, -2.0, -1.0).normalized()),
            makeObb(Vec3(10.0, 20.0, 30.0),
                    rotation3(radians(90.0), xAxis) *
                        scale3(glm::dvec3(1.0, 1.0, 2.0))),
            Vec3(10.0, 20.0, 31.0)
        },
        {
            Ray(Vec3(10.0, 20.0, 40.0), Vec3(0.0, 0.0, -1.0)),
            makeObb(Vec3(10.0, 20.0, 30.0),
                    rotation3(std::atan2(0.5, sqrt2 * 0.5), xAxis) *
                        rotation3(radians(45.0), yAxis)),
            Vec3(10.0, 20.0, 30.0 + sqrt3)
        }
    };

    for (const Case& testCase : cases) {
        const auto intersection = IntersectionTests::rayOBB(testCase.ray, testCase.obb);
        ASSERT_TRUE(intersection.has_value());
        expectVec3Near(testCase.expected, *intersection, 1e-6);
    }
}

TEST(IntersectionTestsTest, RaySphereParametricMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp.
    struct Case {
        Ray ray;
        BoundingSphere sphere;
        double expectedT;
    };

    const Case cases[] = {
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         1.0},
        {Ray(Vec3(0.0, 2.0, 0.0), Vec3(0.0, -1.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         1.0},
        {Ray(Vec3(0.0, 0.0, 2.0), Vec3(0.0, 0.0, -1.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         1.0},
        {Ray(Vec3(1.0, 1.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         1.0},
        {Ray(Vec3(-2.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         1.0},
        {Ray(Vec3(0.0, -2.0, 0.0), Vec3(0.0, 1.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         1.0},
        {Ray(Vec3(0.0, 0.0, -2.0), Vec3(0.0, 0.0, 1.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         1.0},
        {Ray(Vec3(-1.0, -1.0, 0.0), Vec3(1.0, 0.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         1.0},
        {Ray(Vec3(-2.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         -1.0},
        {Ray(Vec3(0.0, -2.0, 0.0), Vec3(0.0, -1.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         -1.0},
        {Ray(Vec3(0.0, 0.0, -2.0), Vec3(0.0, 0.0, -1.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         -1.0},
        {Ray(Vec3(200.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
         BoundingSphere(Vec3::zero(), 5000.0),
         5200.0},
        {Ray(Vec3(200.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)),
         BoundingSphere(Vec3::zero(), 5000.0),
         4800.0},
        {Ray(Vec3(1.0, 0.0, 0.0), Vec3(0.0, 0.0, 1.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         -1.0},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, 0.0, 1.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         -1.0},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, 0.0, -1.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         -1.0},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, 1.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         -1.0},
        {Ray(Vec3(2.0, 0.0, 0.0), Vec3(0.0, -1.0, 0.0)),
         BoundingSphere(Vec3::zero(), 1.0),
         -1.0},
        {Ray(Vec3(202.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         1.0},
        {Ray(Vec3(200.0, 2.0, 0.0), Vec3(0.0, -1.0, 0.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         1.0},
        {Ray(Vec3(200.0, 0.0, 2.0), Vec3(0.0, 0.0, -1.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         1.0},
        {Ray(Vec3(201.0, 1.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         1.0},
        {Ray(Vec3(198.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         1.0},
        {Ray(Vec3(200.0, -2.0, 0.0), Vec3(0.0, 1.0, 0.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         1.0},
        {Ray(Vec3(200.0, 0.0, -2.0), Vec3(0.0, 0.0, 1.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         1.0},
        {Ray(Vec3(199.0, -1.0, 0.0), Vec3(1.0, 0.0, 0.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         1.0},
        {Ray(Vec3(198.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         -1.0},
        {Ray(Vec3(200.0, -2.0, 0.0), Vec3(0.0, -1.0, 0.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         -1.0},
        {Ray(Vec3(200.0, 0.0, -2.0), Vec3(0.0, 0.0, -1.0)),
         BoundingSphere(Vec3(200.0, 0.0, 0.0), 1.0),
         -1.0}
    };

    for (const Case& testCase : cases) {
        std::optional<double> t =
            IntersectionTests::raySphereParametric(testCase.ray, testCase.sphere);
        if (!t) {
            t = -1.0;
        }
        EXPECT_NEAR(testCase.expectedT, *t, 1e-6);
    }
}

TEST(IntersectionTestsTest, RaySphereFiltersNegativeParametricHits) {
    const Ray ray(Vec3(-2.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0));
    const BoundingSphere sphere(Vec3::zero(), 1.0);

    const auto t = IntersectionTests::raySphereParametric(ray, sphere);
    ASSERT_TRUE(t.has_value());
    EXPECT_LT(*t, 0.0);
    EXPECT_FALSE(IntersectionTests::raySphere(ray, sphere).has_value());
}

TEST(IntersectionTestsTest, RaySphereRejectsZeroDistanceTangent) {
    // Source-derived from cesium-native solveQuadratic: a repeated root at
    // exactly 0.0 is treated as no parametric hit.
    const Ray ray(Vec3(1.0, 0.0, 0.0), Vec3::unitZ());
    const BoundingSphere sphere(Vec3::zero(), 1.0);

    EXPECT_FALSE(IntersectionTests::raySphereParametric(ray, sphere).has_value());
    EXPECT_FALSE(IntersectionTests::raySphere(ray, sphere).has_value());
}

TEST(IntersectionTestsTest, RaySphereAcceptsNonZeroRepeatedTangentRoot) {
    // Source-derived from cesium-native solveQuadratic: repeated roots are
    // valid unless the tangent is exactly at the ray origin.
    const Ray ray(Vec3(1.0, 0.0, -2.0), Vec3::unitZ());
    const BoundingSphere sphere(Vec3::zero(), 1.0);

    const auto t = IntersectionTests::raySphereParametric(ray, sphere);

    ASSERT_TRUE(t.has_value());
    EXPECT_DOUBLE_EQ(2.0, *t);
    EXPECT_EQ(Vec3(1.0, 0.0, 0.0),
              IntersectionTests::raySphere(ray, sphere));
}

TEST(IntersectionTestsTest, RaySphereReturnsPointFromCesiumNativeParametricHit) {
    // Source-derived from cesium-native IntersectionTests::raySphere: the
    // wrapper converts the accepted parametric distance back through the ray.
    const Ray ray(Vec3(202.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0));
    const BoundingSphere sphere(Vec3(200.0, 0.0, 0.0), 1.0);

    const auto intersection = IntersectionTests::raySphere(ray, sphere);

    ASSERT_TRUE(intersection.has_value());
    EXPECT_EQ(Vec3(201.0, 0.0, 0.0), *intersection);
}

TEST(IntersectionTestsTest, PointInTriangle2dMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp.
    struct Case {
        glm::dvec2 point;
        glm::dvec2 a;
        glm::dvec2 b;
        glm::dvec2 c;
        bool expected;
    };

    const std::array<glm::dvec2, 3> rightTriangle{
        glm::dvec2(-1.0, 0.0),
        glm::dvec2(0.0, 1.0),
        glm::dvec2(1.0, 0.0)};
    const std::array<glm::dvec2, 3> obtuseTriangle{
        glm::dvec2(2.0, 0.0),
        glm::dvec2(4.0, 1.0),
        glm::dvec2(6.0, 0.0)};

    const Case cases[] = {
        {rightTriangle[2], rightTriangle[0], rightTriangle[1], rightTriangle[2], true},
        {glm::dvec2(0.0, 0.0), rightTriangle[0], rightTriangle[1], rightTriangle[2], true},
        {glm::dvec2(0.2, 0.5), rightTriangle[0], rightTriangle[1], rightTriangle[2], true},
        {glm::dvec2(4.0, 0.3), obtuseTriangle[0], obtuseTriangle[1], obtuseTriangle[2], true},
        {glm::dvec2(-2.0, 0.5), rightTriangle[0], rightTriangle[1], rightTriangle[2], false},
        {glm::dvec2(3.0, -0.5), obtuseTriangle[0], obtuseTriangle[1], obtuseTriangle[2], false},
        {rightTriangle[0], rightTriangle[0], rightTriangle[0], rightTriangle[2], true}
    };

    for (const Case& testCase : cases) {
        EXPECT_EQ(testCase.expected,
                  IntersectionTests::pointInTriangle(testCase.point,
                                                     testCase.a,
                                                     testCase.b,
                                                     testCase.c));
        EXPECT_EQ(testCase.expected,
                  IntersectionTests::pointInTriangle(testCase.point,
                                                     testCase.c,
                                                     testCase.b,
                                                     testCase.a));
    }
}

TEST(IntersectionTestsTest, PointInTriangle3dMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp.
    struct Case {
        Vec3 point;
        Vec3 a;
        Vec3 b;
        Vec3 c;
        bool expected;
    };

    const std::array<Vec3, 3> rightTriangle{
        Vec3(-1.0, 0.0, 0.0),
        Vec3(0.0, 1.0, 0.0),
        Vec3(1.0, 0.0, 0.0)};
    const std::array<Vec3, 3> equilateralTriangle{
        Vec3(1.0, 0.0, 0.0),
        Vec3(0.0, 1.0, 0.0),
        Vec3(0.0, 0.0, 1.0)};

    const Case cases[] = {
        {rightTriangle[2], rightTriangle[0], rightTriangle[1], rightTriangle[2], true},
        {Vec3(0.0, 0.0, 0.0), rightTriangle[0], rightTriangle[1], rightTriangle[2], true},
        {Vec3(0.2, 0.5, 0.0), rightTriangle[0], rightTriangle[1], rightTriangle[2], true},
        {Vec3(0.25, 0.25, 0.5), equilateralTriangle[0], equilateralTriangle[1], equilateralTriangle[2], true},
        {Vec3(-2.0, 0.5, 0.0), rightTriangle[0], rightTriangle[1], rightTriangle[2], false},
        {Vec3(0.2, 0.5, 1.0), rightTriangle[0], rightTriangle[1], rightTriangle[2], false},
        {Vec3(-1.0, 1.5, 0.5), equilateralTriangle[0], equilateralTriangle[1], equilateralTriangle[2], false},
        {Vec3(0.0, 0.0, 0.0), equilateralTriangle[0], equilateralTriangle[1], equilateralTriangle[2], false},
        {rightTriangle[0], rightTriangle[0], rightTriangle[0], rightTriangle[2], false}
    };

    for (const Case& testCase : cases) {
        EXPECT_EQ(testCase.expected,
                  IntersectionTests::pointInTriangle(testCase.point,
                                                     testCase.a,
                                                     testCase.b,
                                                     testCase.c));
        EXPECT_EQ(testCase.expected,
                  IntersectionTests::pointInTriangle(testCase.point,
                                                     testCase.c,
                                                     testCase.b,
                                                     testCase.a));
    }
}

TEST(IntersectionTestsTest, PointInTriangle3dBarycentricMatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp.
    struct Case {
        Vec3 point;
        Vec3 a;
        Vec3 b;
        Vec3 c;
        bool expected;
        Vec3 expectedCoordinates;
    };

    const std::array<Vec3, 3> rightTriangle{
        Vec3(-1.0, 0.0, 0.0),
        Vec3(0.0, 1.0, 0.0),
        Vec3(1.0, 0.0, 0.0)};
    const std::array<Vec3, 3> equilateralTriangle{
        Vec3(1.0, 0.0, 0.0),
        Vec3(0.0, 1.0, 0.0),
        Vec3(0.0, 0.0, 1.0)};

    const Case cases[] = {
        {rightTriangle[2], rightTriangle[0], rightTriangle[1], rightTriangle[2], true, Vec3(0.0, 0.0, 1.0)},
        {Vec3(0.0, 0.0, 0.0), rightTriangle[0], rightTriangle[1], rightTriangle[2], true, Vec3(0.5, 0.0, 0.5)},
        {Vec3(0.0, 0.5, 0.0), rightTriangle[0], rightTriangle[1], rightTriangle[2], true, Vec3(0.25, 0.5, 0.25)},
        {Vec3(0.25, 0.25, 0.5), equilateralTriangle[0], equilateralTriangle[1], equilateralTriangle[2], true, Vec3(0.25, 0.25, 0.5)},
        {Vec3(-2.0, 0.5, 0.0), rightTriangle[0], rightTriangle[1], rightTriangle[2], false, Vec3::zero()},
        {Vec3(0.2, 0.5, 1.0), rightTriangle[0], rightTriangle[1], rightTriangle[2], false, Vec3::zero()},
        {Vec3(-1.0, 1.5, 0.5), equilateralTriangle[0], equilateralTriangle[1], equilateralTriangle[2], false, Vec3::zero()},
        {Vec3(0.0, 0.0, 0.0), equilateralTriangle[0], equilateralTriangle[1], equilateralTriangle[2], false, Vec3::zero()},
        {rightTriangle[0], rightTriangle[0], rightTriangle[0], rightTriangle[2], false, Vec3::zero()}
    };

    for (const Case& testCase : cases) {
        Vec3 barycentricCoordinates;
        const bool result = IntersectionTests::pointInTriangle(testCase.point,
                                                               testCase.a,
                                                               testCase.b,
                                                               testCase.c,
                                                               barycentricCoordinates);
        ASSERT_EQ(testCase.expected, result);
        expectVec3Near(testCase.expectedCoordinates, barycentricCoordinates, 1e-12);

        const bool reverseResult =
            IntersectionTests::pointInTriangle(testCase.point,
                                               testCase.c,
                                               testCase.b,
                                               testCase.a,
                                               barycentricCoordinates);
        ASSERT_EQ(testCase.expected, reverseResult);
        expectVec3Near(Vec3(testCase.expectedCoordinates.z(),
                            testCase.expectedCoordinates.y(),
                            testCase.expectedCoordinates.x()),
                       barycentricCoordinates,
                       1e-12);
    }
}

TEST(IntersectionTestsTest, PointInTriangle3dFalseCasesPreserveBarycentricOutput) {
    // Cesium-native only writes barycentricCoordinates after all inside tests
    // pass; degenerate triangles and outside points leave the output untouched.
    const Vec3 sentinel(9.0, 8.0, 7.0);
    Vec3 barycentricCoordinates = sentinel;

    EXPECT_FALSE(IntersectionTests::pointInTriangle(Vec3(0.0, 0.0, 0.0),
                                                    Vec3(0.0, 0.0, 0.0),
                                                    Vec3(0.0, 0.0, 0.0),
                                                    Vec3(1.0, 0.0, 0.0),
                                                    barycentricCoordinates));
    expectVec3Near(sentinel, barycentricCoordinates, 0.0);

    barycentricCoordinates = sentinel;
    EXPECT_FALSE(IntersectionTests::pointInTriangle(Vec3(-2.0, 0.5, 0.0),
                                                    Vec3(-1.0, 0.0, 0.0),
                                                    Vec3(0.0, 1.0, 0.0),
                                                    Vec3(1.0, 0.0, 0.0),
                                                    barycentricCoordinates));
    expectVec3Near(sentinel, barycentricCoordinates, 0.0);
}
