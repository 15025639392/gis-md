#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/environment/SunDirection.h"
#include "earth_engine/environment/TimeController.h"

using namespace earth_engine;

// ============================================================
// 太阳方向合理性测试
// ============================================================

TEST(SunDirectionTest, OutputIsUnitVector) {
    // 夏至前后
    double jd = unixToJulian(1718967600.0);  // 2024-06-21 12:00 UTC
    Vec3 dir = SunDirection::compute(jd);
    double len = dir.length();
    EXPECT_NEAR(1.0, len, 1e-9);
}

TEST(SunDirectionTest, JuneSolsticeSunAboveEquator) {
    // 夏至：太阳偏北（赤纬 +23.4°）
    double jd = unixToJulian(1718967600.0);  // 2024-06-21
    double elev = SunDirection::elevation(jd);
    EXPECT_GT(elev, 0.35);   // > 20°（赤纬 ≈ 23.4°）
    EXPECT_LT(elev, 0.45);   // < 26°
}

TEST(SunDirectionTest, DecemberSolsticeSunBelowEquator) {
    // 冬至：太阳偏南（赤纬 -23.4°）
    double jd = unixToJulian(1734865200.0);  // 2024-12-21 12:00 UTC
    double elev = SunDirection::elevation(jd);
    EXPECT_LT(elev, -0.35);  // < -20°
    EXPECT_GT(elev, -0.45);  // > -26°
}

TEST(SunDirectionTest, EquinoxSunNearEquator) {
    // 春分：太阳在赤道附近
    double jd = unixToJulian(1710892800.0);  // 2024-03-20 00:00 UTC approx
    double elev = SunDirection::elevation(jd);
    EXPECT_NEAR(0.0, elev, 0.08);  // within ~5°
}

TEST(SunDirectionTest, DirectionChangesWithTime) {
    double jd0 = unixToJulian(1718967600.0);       // noon
    double jd1 = unixToJulian(1718967600.0 + 43200.0); // +12 hours
    Vec3 d0 = SunDirection::compute(jd0);
    Vec3 d1 = SunDirection::compute(jd1);

    // 12 小时后太阳方向应显著不同
    double dot = d0.x() * d1.x() + d0.y() * d1.y() + d0.z() * d1.z();
    EXPECT_LT(dot, 0.5);  // 角度 > 60°
}

TEST(SunDirectionTest, CosIncidenceRange) {
    double jd = unixToJulian(1718967600.0);
    // 中午在赤道本初子午线，cosIncidence 应接近 1
    double ci0 = SunDirection::cosIncidence(jd, 0.0, 0.0);
    EXPECT_GT(ci0, 0.0);
    EXPECT_LE(ci0, 1.0);

    // 夜面（经度 180°）应接近 0
    double ci180 = SunDirection::cosIncidence(jd, M_PI, 0.0);
    EXPECT_NEAR(0.0, ci180, 1e-6);
}
