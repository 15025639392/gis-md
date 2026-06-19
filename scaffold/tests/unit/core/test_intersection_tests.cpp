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
