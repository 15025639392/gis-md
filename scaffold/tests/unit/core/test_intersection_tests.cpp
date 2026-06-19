#include <gtest/gtest.h>

#include "earth_engine/core/math/AxisAlignedBox.h"
#include "earth_engine/core/math/BoundingSphere.h"
#include "earth_engine/core/math/IntersectionTests.h"
#include "earth_engine/core/math/OrientedBoundingBox.h"
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Ray.h"
#include "earth_engine/core/math/Vec3.h"

#include <cmath>
#include <glm/ext/matrix_double3x3.hpp>
#include <glm/ext/matrix_transform.hpp>
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
