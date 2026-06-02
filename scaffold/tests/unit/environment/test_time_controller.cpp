#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/environment/TimeController.h"

using namespace earth_engine;

TEST(TimeControllerTest, DefaultConstructsWithCurrentTime) {
    TimeController tc;
    // 应返回一个合理的 Julian Date（2024+ → JD > 2460000）
    EXPECT_GT(tc.julianDate(), 2460000.0);
    EXPECT_LT(tc.julianDate(), 2500000.0);  // < year 2100
}

TEST(TimeControllerTest, SetAndGetJulianDate) {
    TimeController tc;
    double jd = 2451545.0;  // J2000.0
    tc.setJulianDate(jd);
    EXPECT_DOUBLE_EQ(jd, tc.julianDate());
}

TEST(TimeControllerTest, UnixConversion) {
    // 2024-06-21 12:00:00 UTC ≈ 1718967600
    double ts = 1718967600.0;
    double jd = unixToJulian(ts);
    double back = julianToUnix(jd);
    EXPECT_NEAR(ts, back, 1.0);  // 1-second tolerance
}

TEST(TimeControllerTest, SetUnixTimestamp) {
    TimeController tc;
    tc.setUnixTimestamp(1718967600.0);
    EXPECT_GT(tc.julianDate(), 2460000.0);
}

TEST(TimeControllerTest, AdvanceTime) {
    TimeController tc;
    tc.setJulianDate(2451545.0);
    tc.advanceSeconds(86400.0);  // +1 day
    EXPECT_NEAR(2451546.0, tc.julianDate(), 1e-9);
}

TEST(TimeControllerTest, AdvanceNegative) {
    TimeController tc;
    tc.setJulianDate(2451545.0);
    tc.advanceSeconds(-43200.0);  // -12 hours
    EXPECT_NEAR(2451544.5, tc.julianDate(), 1e-9);
}

TEST(JulianConversionTest, KnownEpoch) {
    // Unix epoch (1970-01-01 00:00 UTC) = JD 2440587.5
    double jd = unixToJulian(0.0);
    EXPECT_DOUBLE_EQ(2440587.5, jd);
}
