#include <gtest/gtest.h>

#include "earth_engine/core/math/ClipTriangleAtAxisAlignedThreshold.h"

#include <vector>

using namespace earth_engine;

TEST(ClipTriangleAtAxisAlignedThresholdTest, MatchesCesiumNativeCases) {
    // Ported from cesium-native CesiumGeometry/test/TestClipTriangleAtAxisAlignedThreshold.cpp.
    struct Case {
        double threshold;
        bool keepAbove;
        int i0;
        int i1;
        int i2;
        double u0;
        double u1;
        double u2;
        std::vector<TriangleClipVertex> expected;
    };

    const Case cases[] = {
        {0.1, false, 0, 1, 2, 0.2, 0.3, 0.4, {}},
        {0.1, true, 0, 1, 2, 0.2, 0.3, 0.4, {0, 1, 2}},
        {0.5,
         false,
         0,
         1,
         2,
         0.6,
         0.4,
         0.2,
         {1, 2, InterpolatedVertex{0, 2, 0.25}, InterpolatedVertex{0, 1, 0.5}}},
        {0.5,
         true,
         0,
         1,
         2,
         0.4,
         0.6,
         0.8,
         {1, 2, InterpolatedVertex{0, 2, 0.25}, InterpolatedVertex{0, 1, 0.5}}},
        {0.5,
         false,
         0,
         1,
         2,
         0.2,
         0.6,
         0.4,
         {2, 0, InterpolatedVertex{1, 0, 0.25}, InterpolatedVertex{1, 2, 0.5}}},
        {0.5,
         true,
         0,
         1,
         2,
         0.8,
         0.4,
         0.6,
         {2, 0, InterpolatedVertex{1, 0, 0.25}, InterpolatedVertex{1, 2, 0.5}}},
        {0.5,
         false,
         0,
         1,
         2,
         0.4,
         0.2,
         0.6,
         {0, 1, InterpolatedVertex{2, 1, 0.25}, InterpolatedVertex{2, 0, 0.5}}},
        {0.5,
         true,
         0,
         1,
         2,
         0.6,
         0.8,
         0.4,
         {0, 1, InterpolatedVertex{2, 1, 0.25}, InterpolatedVertex{2, 0, 0.5}}},
        {0.5,
         true,
         0,
         1,
         2,
         0.8,
         0.4,
         0.6,
         {2, 0, InterpolatedVertex{1, 0, 0.25}, InterpolatedVertex{1, 2, 0.5}}},
        {0.5,
         false,
         0,
         1,
         2,
         0.4,
         0.6,
         0.8,
         {0, InterpolatedVertex{1, 0, 0.5}, InterpolatedVertex{2, 0, 0.75}}},
        {0.5,
         true,
         0,
         1,
         2,
         0.6,
         0.4,
         0.2,
         {0, InterpolatedVertex{1, 0, 0.5}, InterpolatedVertex{2, 0, 0.75}}},
        {0.5,
         false,
         0,
         1,
         2,
         0.8,
         0.4,
         0.6,
         {1, InterpolatedVertex{2, 1, 0.5}, InterpolatedVertex{0, 1, 0.75}}},
        {0.5,
         true,
         0,
         1,
         2,
         0.2,
         0.6,
         0.4,
         {1, InterpolatedVertex{2, 1, 0.5}, InterpolatedVertex{0, 1, 0.75}}},
        {0.5,
         false,
         0,
         1,
         2,
         0.6,
         0.8,
         0.4,
         {2, InterpolatedVertex{0, 2, 0.5}, InterpolatedVertex{1, 2, 0.75}}},
        {0.5,
         true,
         0,
         1,
         2,
         0.4,
         0.2,
         0.6,
         {2, InterpolatedVertex{0, 2, 0.5}, InterpolatedVertex{1, 2, 0.75}}}
    };

    for (const Case& testCase : cases) {
        std::vector<TriangleClipVertex> actual;
        clipTriangleAtAxisAlignedThreshold(testCase.threshold,
                                           testCase.keepAbove,
                                           testCase.i0,
                                           testCase.i1,
                                           testCase.i2,
                                           testCase.u0,
                                           testCase.u1,
                                           testCase.u2,
                                           actual);
        EXPECT_EQ(testCase.expected, actual);
    }
}

TEST(ClipTriangleAtAxisAlignedThresholdTest, AppendsToExistingResult) {
    std::vector<TriangleClipVertex> actual{99};

    clipTriangleAtAxisAlignedThreshold(0.1,
                                       true,
                                       0,
                                       1,
                                       2,
                                       0.2,
                                       0.3,
                                       0.4,
                                       actual);

    const std::vector<TriangleClipVertex> expected{99, 0, 1, 2};
    EXPECT_EQ(expected, actual);
}

TEST(ClipTriangleAtAxisAlignedThresholdTest, KeepsThresholdVertexAndSkipsCoincidentInterpolation) {
    std::vector<TriangleClipVertex> actual;

    clipTriangleAtAxisAlignedThreshold(0.5,
                                       true,
                                       0,
                                       1,
                                       2,
                                       0.5,
                                       0.4,
                                       0.6,
                                       actual);

    const std::vector<TriangleClipVertex> expected{
        2,
        0,
        InterpolatedVertex{1, 2, 0.5}};
    EXPECT_EQ(expected, actual);
}
