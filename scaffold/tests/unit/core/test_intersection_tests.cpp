#include <gtest/gtest.h>

#include "earth_engine/core/math/AxisAlignedBox.h"
#include "earth_engine/core/math/IntersectionTests.h"
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Ray.h"
#include "earth_engine/core/math/Vec3.h"

#include <cmath>
#include <optional>

using namespace earth_engine;

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
