#include <gtest/gtest.h>

#include "earth_engine/core/math/AttributeCompression.h"
#include "earth_engine/core/math/MathUtils.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <glm/ext/vector_double3.hpp>
#include <glm/geometric.hpp>

using namespace earth_engine;

namespace {

bool equalsEpsilon(const glm::dvec3& left,
                   const glm::dvec3& right,
                   double epsilon) {
    return MathUtils::equalsEpsilon(left.x, right.x, epsilon) &&
           MathUtils::equalsEpsilon(left.y, right.y, epsilon) &&
           MathUtils::equalsEpsilon(left.z, right.z, epsilon);
}

} // namespace

TEST(AttributeCompressionTest, OctDecodeMatchesCesiumNative) {
    const std::array<std::pair<uint8_t, uint8_t>, 14> input{{
        {128, 128},
        {255, 255},
        {128, 255},
        {128, 0},
        {255, 128},
        {0, 128},
        {170, 170},
        {170, 85},
        {85, 85},
        {85, 170},
        {213, 213},
        {213, 42},
        {42, 42},
        {42, 213},
    }};
    const std::array<glm::dvec3, 14> expected{{
        glm::dvec3(0.0, 0.0, 1.0),
        glm::dvec3(0.0, 0.0, -1.0),
        glm::dvec3(0.0, 1.0, 0.0),
        glm::dvec3(0.0, -1.0, 0.0),
        glm::dvec3(1.0, 0.0, 0.0),
        glm::dvec3(-1.0, 0.0, 0.0),
        glm::normalize(glm::dvec3(1.0, 1.0, 1.0)),
        glm::normalize(glm::dvec3(1.0, -1.0, 1.0)),
        glm::normalize(glm::dvec3(-1.0, -1.0, 1.0)),
        glm::normalize(glm::dvec3(-1.0, 1.0, 1.0)),
        glm::normalize(glm::dvec3(1.0, 1.0, -1.0)),
        glm::normalize(glm::dvec3(1.0, -1.0, -1.0)),
        glm::normalize(glm::dvec3(-1.0, -1.0, -1.0)),
        glm::normalize(glm::dvec3(-1.0, 1.0, -1.0)),
    }};

    for (size_t i = 0; i < expected.size(); ++i) {
        const glm::dvec3 value =
            AttributeCompression::octDecode(input[i].first, input[i].second);
        EXPECT_TRUE(equalsEpsilon(value, expected[i], MathUtils::Epsilon1))
            << "index " << i;
    }
}

TEST(AttributeCompressionTest, OctDecodeInRangeSupportsNonByteSnormRanges) {
    // Source-derived from cesium-native AttributeCompression::octDecodeInRange:
    // the same oct normal decode is templated for unsigned SNORM ranges beyond
    // the uint8_t range used by octDecode.
    const glm::dvec3 positiveZ = AttributeCompression::octDecodeInRange<uint16_t>(
        682u,
        341u,
        1023u);
    EXPECT_TRUE(equalsEpsilon(
        positiveZ,
        glm::normalize(glm::dvec3(1.0, -1.0, 1.0)),
        MathUtils::Epsilon3));

    const glm::dvec3 negativeZ = AttributeCompression::octDecodeInRange<uint16_t>(
        1023u,
        0u,
        1023u);
    EXPECT_TRUE(equalsEpsilon(
        negativeZ,
        glm::dvec3(0.0, 0.0, -1.0),
        MathUtils::Epsilon3));
}

TEST(AttributeCompressionTest, DecodeRGB565MatchesCesiumNative) {
    const std::array<uint16_t, 4> input{{
        0u,
        2081u,
        33800u,
        65535u,
    }};
    const std::array<glm::dvec3, 4> expected{{
        glm::dvec3(0.0),
        glm::dvec3(1.0 / 31.0, 1.0 / 63.0, 1.0 / 31.0),
        glm::dvec3(16.0 / 31.0, 32.0 / 63.0, 8.0 / 31.0),
        glm::dvec3(1.0),
    }};

    for (size_t i = 0; i < expected.size(); ++i) {
        const glm::dvec3 value = AttributeCompression::decodeRGB565(input[i]);
        EXPECT_TRUE(equalsEpsilon(value, expected[i], MathUtils::Epsilon6))
            << "index " << i;
    }
}
