#include <gtest/gtest.h>

#include "earth_engine/core/math/ClipTriangleAtAxisAlignedThreshold.h"

#include <limits>
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
    std::vector<TriangleClipVertex> actual{
        InterpolatedVertex{9, 10, 0.25},
    };

    clipTriangleAtAxisAlignedThreshold(0.1,
                                       true,
                                       0,
                                       1,
                                       2,
                                       0.2,
                                       0.3,
                                       0.4,
                                       actual);

    const std::vector<TriangleClipVertex> expected{
        InterpolatedVertex{9, 10, 0.25},
        0,
        1,
        2,
    };
    EXPECT_EQ(expected, actual);
}

TEST(ClipTriangleAtAxisAlignedThresholdTest, InterpolatedVertexEqualityUsesCesiumNativeEpsilon) {
    const InterpolatedVertex vertex{1, 2, 0.25};

    EXPECT_EQ(vertex, (InterpolatedVertex{1, 2, 0.25 + std::numeric_limits<double>::epsilon()}));
    EXPECT_NE(vertex, (InterpolatedVertex{1, 2, 0.25 + 2.0 * std::numeric_limits<double>::epsilon()}));
    EXPECT_NE(vertex, (InterpolatedVertex{2, 1, 0.25}));
}

TEST(ClipTriangleAtAxisAlignedThresholdTest, KeepsThresholdVertexAndSkipsCoincidentInterpolation) {
    // Source-derived from cesium-native
    // clipTriangleAtAxisAlignedThreshold.cpp: when two vertices are behind,
    // the remaining vertex is emitted only if it is not exactly on the
    // threshold. Equality is therefore kept without a duplicate interpolation.
    struct Case {
        bool keepAbove;
        double u0;
        double u1;
        double u2;
        std::vector<TriangleClipVertex> expected;
    };

    const Case cases[] = {
        {
            true,
            0.5,
            0.4,
            0.6,
            {2, 0, InterpolatedVertex{1, 2, 0.5}}
        },
        {
            false,
            0.5,
            0.6,
            0.4,
            {2, 0, InterpolatedVertex{1, 2, 0.5}}
        },
        {
            true,
            0.6,
            0.5,
            0.4,
            {0, 1, InterpolatedVertex{2, 0, 0.5}}
        },
        {
            false,
            0.4,
            0.5,
            0.6,
            {0, 1, InterpolatedVertex{2, 0, 0.5}}
        },
        {
            true,
            0.4,
            0.6,
            0.5,
            {1, 2, InterpolatedVertex{0, 1, 0.5}}
        },
        {
            false,
            0.6,
            0.4,
            0.5,
            {1, 2, InterpolatedVertex{0, 1, 0.5}}
        }
    };

    for (const Case& testCase : cases) {
        std::vector<TriangleClipVertex> actual;

        clipTriangleAtAxisAlignedThreshold(0.5,
                                           testCase.keepAbove,
                                           0,
                                           1,
                                           2,
                                           testCase.u0,
                                           testCase.u1,
                                           testCase.u2,
                                           actual);

        EXPECT_EQ(testCase.expected, actual);
    }
}
