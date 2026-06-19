#include <gtest/gtest.h>

#include "earth_engine/core/math/MathUtils.h"

using namespace earth_engine;

TEST(MathUtilsTest, LerpMatchesCesiumNative) {
    EXPECT_DOUBLE_EQ(1.0, MathUtils::lerp(1.0, 2.0, 0.0));
    EXPECT_DOUBLE_EQ(1.5, MathUtils::lerp(1.0, 2.0, 0.5));
    EXPECT_DOUBLE_EQ(2.0, MathUtils::lerp(1.0, 2.0, 1.0));
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
    EXPECT_TRUE(MathUtils::equalsEpsilon(
        -MathUtils::OnePi + 0.1,
        MathUtils::negativePiToPi(MathUtils::OnePi + 0.1),
        MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(0.0,
                     MathUtils::negativePiToPi(2.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::negativePiToPi(3.0 * MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::negativePiToPi(-3.0 * MathUtils::OnePi));
}

TEST(MathUtilsTest, ZeroToTwoPiMatchesCesiumNative) {
    EXPECT_DOUBLE_EQ(0.0, MathUtils::zeroToTwoPi(0.0));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(MathUtils::OnePi));
    EXPECT_DOUBLE_EQ(MathUtils::OnePi,
                     MathUtils::zeroToTwoPi(-MathUtils::OnePi));
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
}

TEST(MathUtilsTest, ModMatchesCesiumNativeSignedCases) {
    EXPECT_DOUBLE_EQ(0.0, MathUtils::mod(0.0, 1.0));
    EXPECT_DOUBLE_EQ(0.5, MathUtils::mod(0.5, 1.0));
    EXPECT_DOUBLE_EQ(0.0, MathUtils::mod(1.0, 1.0));
    EXPECT_TRUE(MathUtils::equalsEpsilon(0.9,
                                         MathUtils::mod(-0.1, 1.0),
                                         MathUtils::Epsilon15));
    EXPECT_DOUBLE_EQ(-0.5, MathUtils::mod(0.5, -1.0));
    EXPECT_TRUE(MathUtils::equalsEpsilon(-0.1,
                                         MathUtils::mod(-1.1, -1.0),
                                         MathUtils::Epsilon15));
}
