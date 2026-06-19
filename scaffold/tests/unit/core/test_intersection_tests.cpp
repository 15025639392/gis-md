#include <gtest/gtest.h>

#include "earth_engine/core/math/IntersectionTests.h"
#include "earth_engine/core/math/Plane.h"
#include "earth_engine/core/math/Ray.h"
#include "earth_engine/core/math/Vec3.h"

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
