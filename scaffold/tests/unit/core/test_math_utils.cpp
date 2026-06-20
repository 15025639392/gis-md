#include <gtest/gtest.h>

#include "earth_engine/core/math/AttributeCompression.h"
#include "earth_engine/core/math/MathUtils.h"
#include "earth_engine/core/math/Mat4.h"
#include "earth_engine/core/math/Vec3.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

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

TEST(MathUtilsTest, ConstantsAndAngleConversionsMatchCesiumNative) {
    EXPECT_DOUBLE_EQ(1e-1, MathUtils::Epsilon1);
    EXPECT_DOUBLE_EQ(1e-12, MathUtils::Epsilon12);
    EXPECT_DOUBLE_EQ(1e-21, MathUtils::Epsilon21);
    EXPECT_DOUBLE_EQ(3.14159265358979323846, MathUtils::OnePi);
    EXPECT_DOUBLE_EQ(MathUtils::OnePi * 2.0, MathUtils::TwoPi);
    EXPECT_DOUBLE_EQ(MathUtils::OnePi / 2.0, MathUtils::PiOverTwo);
    EXPECT_DOUBLE_EQ(MathUtils::OnePi / 4.0, MathUtils::PiOverFour);
    EXPECT_DOUBLE_EQ(1.61803398874989484, MathUtils::GoldenRatio);

    EXPECT_DOUBLE_EQ(MathUtils::OnePi, MathUtils::degreesToRadians(180.0));
    EXPECT_DOUBLE_EQ(180.0, MathUtils::radiansToDegrees(MathUtils::OnePi));
}

TEST(MathUtilsTest, LerpMatchesCesiumNative) {
    EXPECT_DOUBLE_EQ(1.0, MathUtils::lerp(1.0, 2.0, 0.0));
    EXPECT_DOUBLE_EQ(1.5, MathUtils::lerp(1.0, 2.0, 0.5));
    EXPECT_DOUBLE_EQ(2.0, MathUtils::lerp(1.0, 2.0, 1.0));
}

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

TEST(MathUtilsTest, EqualsEpsilonMatchesCesiumNative) {
    EXPECT_TRUE(MathUtils::equalsEpsilon(0.0, 0.01, MathUtils::Epsilon2));
    EXPECT_FALSE(MathUtils::equalsEpsilon(0.0, 0.1, MathUtils::Epsilon2));
    EXPECT_TRUE(MathUtils::equalsEpsilon(3699175.1634344,
                                         3699175.2,
                                         MathUtils::Epsilon7));
    EXPECT_FALSE(MathUtils::equalsEpsilon(3699175.1634344,
                                          3699175.2,
                                          MathUtils::Epsilon9));
}

TEST(MathUtilsTest, EqualsEpsilonVec3MatchesCesiumNativeComponentSemantics) {
    const Vec3 base(1000.0, -2000.0, 0.0);
    const Vec3 relativeMatch(1000.0005, -2000.0005, 5e-7);
    const Vec3 relativeMiss(1000.0005, -2000.0005, 2e-6);

    EXPECT_TRUE(MathUtils::equalsEpsilon(base, relativeMatch, MathUtils::Epsilon6));
    EXPECT_FALSE(MathUtils::equalsEpsilon(base, relativeMiss, MathUtils::Epsilon6));

    EXPECT_TRUE(MathUtils::equalsEpsilon(base,
                                        Vec3(1000.0, -2000.0, 1e-5),
                                        MathUtils::Epsilon12,
                                        MathUtils::Epsilon4));
    EXPECT_FALSE(MathUtils::equalsEpsilon(base,
                                         Vec3(1000.0, -2000.0, 1e-3),
                                         MathUtils::Epsilon12,
                                         MathUtils::Epsilon4));
}

TEST(MathUtilsTest, EqualsEpsilonMat4MatchesCesiumNativeColumnSemantics) {
    Mat4 base = Mat4::identity();
    Mat4 within = base;
    within(0, 0) += 5e-7;
    within(2, 3) = -5e-7;

    Mat4 outside = within;
    outside(1, 2) = 2e-6;

    EXPECT_TRUE(MathUtils::equalsEpsilon(base, within, MathUtils::Epsilon6));
    EXPECT_FALSE(MathUtils::equalsEpsilon(base, outside, MathUtils::Epsilon6));

    EXPECT_TRUE(MathUtils::equalsEpsilon(base,
                                        outside,
                                        MathUtils::Epsilon12,
                                        MathUtils::Epsilon5));
}

TEST(MathUtilsTest, RelativeEpsilonAndSignMatchCesiumNativeSourceSemantics) {
    EXPECT_DOUBLE_EQ(0.2,
                     MathUtils::relativeEpsilonToAbsolute(10.0, -20.0, 0.01));
    EXPECT_DOUBLE_EQ(0.0,
                     MathUtils::relativeEpsilonToAbsolute(0.0, 0.0, 0.01));

    EXPECT_EQ(Vec3(0.1, 0.2, 0.003),
              MathUtils::relativeEpsilonToAbsolute(Vec3(10.0, -20.0, 0.0),
                                                   Vec3(-5.0, 3.0, -0.3),
                                                   0.01));

    EXPECT_DOUBLE_EQ(1.0, MathUtils::sign(42.0));
    EXPECT_DOUBLE_EQ(-1.0, MathUtils::sign(-42.0));
    EXPECT_DOUBLE_EQ(0.0, MathUtils::sign(0.0));
    const double negativeZeroSign = MathUtils::sign(-0.0);
    EXPECT_DOUBLE_EQ(-0.0, negativeZeroSign);
    EXPECT_TRUE(std::signbit(negativeZeroSign));
    EXPECT_TRUE(std::isnan(MathUtils::sign(std::numeric_limits<double>::quiet_NaN())));

    EXPECT_DOUBLE_EQ(1.0, MathUtils::signNotZero(42.0));
    EXPECT_DOUBLE_EQ(-1.0, MathUtils::signNotZero(-42.0));
    EXPECT_DOUBLE_EQ(1.0, MathUtils::signNotZero(0.0));
    EXPECT_DOUBLE_EQ(1.0, MathUtils::signNotZero(-0.0));
}

TEST(MathUtilsTest, RoundUpAndRoundDownMatchCesiumNative) {
    EXPECT_DOUBLE_EQ(1.0, MathUtils::roundUp(1.0, 0.01));
    EXPECT_DOUBLE_EQ(1.0, MathUtils::roundDown(1.0, 0.01));

    EXPECT_DOUBLE_EQ(2.0, MathUtils::roundUp(1.01, 0.01));
    EXPECT_DOUBLE_EQ(1.0, MathUtils::roundDown(1.99, 0.01));

    EXPECT_DOUBLE_EQ(1.0, MathUtils::roundUp(1.005, 0.01));
    EXPECT_DOUBLE_EQ(2.0, MathUtils::roundDown(1.995, 0.01));

    EXPECT_DOUBLE_EQ(-1.0, MathUtils::roundUp(-1.0, 0.01));
    EXPECT_DOUBLE_EQ(-1.0, MathUtils::roundDown(-1.0, 0.01));

    EXPECT_DOUBLE_EQ(-1.0, MathUtils::roundUp(-1.99, 0.01));
    EXPECT_DOUBLE_EQ(-2.0, MathUtils::roundDown(-1.01, 0.01));

    EXPECT_DOUBLE_EQ(-2.0, MathUtils::roundUp(-1.995, 0.01));
    EXPECT_DOUBLE_EQ(-1.0, MathUtils::roundDown(-1.005, 0.01));
}

TEST(MathUtilsTest, ClampAndSNormMatchCesiumNativeSourceSemantics) {
    EXPECT_DOUBLE_EQ(0.0, MathUtils::clamp(-1.0, 0.0, 1.0));
    EXPECT_DOUBLE_EQ(0.5, MathUtils::clamp(0.5, 0.0, 1.0));
    EXPECT_DOUBLE_EQ(1.0, MathUtils::clamp(2.0, 0.0, 1.0));

    EXPECT_DOUBLE_EQ(0.0, MathUtils::toSNorm(-1.0));
    EXPECT_DOUBLE_EQ(128.0, MathUtils::toSNorm(0.0));
    EXPECT_DOUBLE_EQ(255.0, MathUtils::toSNorm(1.0));
    EXPECT_DOUBLE_EQ(255.0, MathUtils::toSNorm(2.0));
    EXPECT_DOUBLE_EQ(0.0, MathUtils::toSNorm(-2.0));

    EXPECT_DOUBLE_EQ(0.0, MathUtils::toSNorm(-1.0, 1023.0));
    EXPECT_DOUBLE_EQ(512.0, MathUtils::toSNorm(0.0, 1023.0));
    EXPECT_DOUBLE_EQ(1023.0, MathUtils::toSNorm(1.0, 1023.0));

    EXPECT_DOUBLE_EQ(-1.0, MathUtils::fromSNorm(0.0));
    EXPECT_DOUBLE_EQ(1.0, MathUtils::fromSNorm(255.0));
    EXPECT_NEAR(0.0039215686274509665, MathUtils::fromSNorm(128.0), 1e-16);
    EXPECT_DOUBLE_EQ(-1.0, MathUtils::fromSNorm(-1.0));
    EXPECT_DOUBLE_EQ(1.0, MathUtils::fromSNorm(300.0));
    EXPECT_NEAR(0.0009775171065493637,
                MathUtils::fromSNorm(512.0, 1023.0),
                1e-16);
}

TEST(MathUtilsTest, ConvertLongitudeRangeMatchesCesiumNative) {
    EXPECT_DOUBLE_EQ(MathUtils::degreesToRadians(-90.0),
                     MathUtils::convertLongitudeRange(
                         MathUtils::degreesToRadians(270.0)));
    EXPECT_DOUBLE_EQ(0.0, MathUtils::convertLongitudeRange(0.0));
    EXPECT_DOUBLE_EQ(-MathUtils::OnePi,
                     MathUtils::convertLongitudeRange(MathUtils::OnePi));
}

TEST(MathUtilsTest, NegativePiToPiMatchesCesiumNative) {
    EXPECT_DOUBLE_EQ(0.0, MathUtils::negativePiToPi(0.0));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::negativePiToPi(MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(-MathUtils::OnePi,
                     MathUtils::negativePiToPi(-MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi - 1.0,
                     MathUtils::negativePiToPi(MathUtils::OnePi - 1.0));
    EXPECT_DOUBLE_EQ(-MathUtils::OnePi + 1.0,
                     MathUtils::negativePiToPi(-MathUtils::OnePi + 1.0));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi - 0.1,
                     MathUtils::negativePiToPi(MathUtils::OnePi - 0.1));
    EXPECT_DOUBLE_EQ(-MathUtils::OnePi + 0.1,
                     MathUtils::negativePiToPi(-MathUtils::OnePi + 0.1));
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        -MathUtils::OnePi + 0.1,
        MathUtils::negativePiToPi(MathUtils::OnePi + 0.1),
        MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(0.0,
                     MathUtils::negativePiToPi(2.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(0.0,
                     MathUtils::negativePiToPi(-2.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::negativePiToPi(3.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::negativePiToPi(-3.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(0.0,
                     MathUtils::negativePiToPi(4.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(0.0,
                     MathUtils::negativePiToPi(-4.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::negativePiToPi(5.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::negativePiToPi(-5.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(0.0,
                     MathUtils::negativePiToPi(6.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(0.0,
                     MathUtils::negativePiToPi(-6.0 * MathUtils::OnePi));
}

TEST(MathUtilsTest, ZeroToTwoPiMatchesCesiumNative) {
    EXPECT_DOUBLE_EQ(0.0, MathUtils::zeroToTwoPi(0.0));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(-MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi - 1.0,
                     MathUtils::zeroToTwoPi(MathUtils::OnePi - 1.0));
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        MathUtils::OnePi + 1.0,
        MathUtils::zeroToTwoPi(-MathUtils::OnePi + 1.0),
        MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi - 0.1,
                     MathUtils::zeroToTwoPi(MathUtils::OnePi - 0.1));
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        MathUtils::OnePi + 0.1,
        MathUtils::zeroToTwoPi(-MathUtils::OnePi + 0.1),
        MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(2.0 * MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(2.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(2.0 * MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(-2.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(3.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(-3.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(2.0 * MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(4.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(2.0 * MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(-4.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(5.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(-5.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(2.0 * MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(6.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(2.0 * MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(-6.0 * MathUtils::OnePi));
}

TEST(MathUtilsTest, ModMatchesCesiumNativeSignedCases) {
    EXPECT_DOUBLE_EQ(0.0, MathUtils::mod(0.0, 1.0));
    EXPECT_DOUBLE_EQ(0.1, MathUtils::mod(0.1, 1.0));
    EXPECT_DOUBLE_EQ(0.5, MathUtils::mod(0.5, 1.0));
    EXPECT_DOUBLE_EQ(0.0, MathUtils::mod(1.0, 1.0));
    EXPECT_TRUE(MathUtils::equalsEpsilon(0.1,
                                         MathUtils::mod(1.1, 1.0),
                                         MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(0.0, MathUtils::mod(-0.0, 1.0));
    EXPECT_TRUE(MathUtils::equalsEpsilon(0.9,
                                         MathUtils::mod(-0.1, 1.0),
                                         MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(0.5, MathUtils::mod(-0.5, 1.0));
    EXPECT_DOUBLE_EQ(0.0, MathUtils::mod(-1.0, 1.0));
    EXPECT_TRUE(MathUtils::equalsEpsilon(0.9,
                                         MathUtils::mod(-1.1, 1.0),
                                         MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(-0.0, MathUtils::mod(0.0, -1.0));
    EXPECT_TRUE(MathUtils::equalsEpsilon(-0.9,
                                         MathUtils::mod(0.1, -1.0),
                                         MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(-0.5, MathUtils::mod(0.5, -1.0));
    EXPECT_DOUBLE_EQ(-0.0, MathUtils::mod(1.0, -1.0));
    EXPECT_TRUE(MathUtils::equalsEpsilon(-0.9,
                                         MathUtils::mod(1.1, -1.0),
                                         MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(-0.0, MathUtils::mod(-0.0, -1.0));
    EXPECT_DOUBLE_EQ(-0.1, MathUtils::mod(-0.1, -1.0));
    EXPECT_DOUBLE_EQ(-0.5, MathUtils::mod(-0.5, -1.0));
    EXPECT_DOUBLE_EQ(-0.0, MathUtils::mod(-1.0, -1.0));
    EXPECT_TRUE(MathUtils::equalsEpsilon(-0.1,
                                         MathUtils::mod(-1.1, -1.0),
                                         MathUtils::Epsilon15));
}

TEST(MathUtilsTest, ModPreservesCesiumNativeEarlyReturnSignedZero) {
    const double positiveZero = MathUtils::mod(0.0, 1.0);
    const double negativeZero = MathUtils::mod(-0.0, -1.0);

    EXPECT_DOUBLE_EQ(0.0, positiveZero);
    EXPECT_FALSE(std::signbit(positiveZero));

    EXPECT_DOUBLE_EQ(-0.0, negativeZero);
    EXPECT_TRUE(std::signbit(negativeZero));
}
