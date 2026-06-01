#include <gtest/gtest.h>
#include <cmath>
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Transforms.h"
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

constexpr double kEpsilon = 1e-6;  // 容差：~0.1 mm 量级

// ============================================================
// WGS84 常量
// ============================================================

TEST(EllipsoidTest, Wgs84Constants) {
    const auto& e = Ellipsoid::WGS84();
    EXPECT_DOUBLE_EQ(6378137.0, e.semiMajorAxis());
    EXPECT_DOUBLE_EQ(6356752.314245, e.semiMinorAxis());
    EXPECT_NEAR(1.0 / 298.257223563, e.flattening(), 1e-12);
}

// ============================================================
// Cartographic → ECEF → Cartographic 往返
// ============================================================

TEST(EllipsoidTest, RoundtripEquator) {
    // 赤道，本初子午线，高度 0
    auto cart = Cartographic::fromRadians(0, 0, 0);
    const auto& e = Ellipsoid::WGS84();

    Vec3 ecef = e.cartographicToCartesian(cart);
    auto back = e.cartesianToCartographic(ecef);

    EXPECT_NEAR(0, back.longitude(), kEpsilon);
    EXPECT_NEAR(0, back.latitude(), kEpsilon);
    EXPECT_NEAR(0, back.height(), kEpsilon);
}

TEST(EllipsoidTest, RoundtripBeijing) {
    // 北京近似：116.397°E, 39.908°N, 高度 50m
    auto cart = Cartographic::fromDegrees(116.397, 39.908, 50);
    const auto& e = Ellipsoid::WGS84();

    Vec3 ecef = e.cartographicToCartesian(cart);
    auto back = e.cartesianToCartographic(ecef);

    EXPECT_NEAR(cart.longitude(), back.longitude(), kEpsilon);
    EXPECT_NEAR(cart.latitude(), back.latitude(), kEpsilon);
    EXPECT_NEAR(cart.height(), back.height(), kEpsilon);
}

TEST(EllipsoidTest, RoundtripHighAltitude) {
    // 海拔 10000m（GPS 卫星轨道高度以下正常值）
    auto cart = Cartographic::fromDegrees(120, 30, 10000);
    const auto& e = Ellipsoid::WGS84();

    Vec3 ecef = e.cartographicToCartesian(cart);
    auto back = e.cartesianToCartographic(ecef);

    EXPECT_NEAR(cart.longitude(), back.longitude(), kEpsilon);
    EXPECT_NEAR(cart.latitude(), back.latitude(), kEpsilon);
    EXPECT_NEAR(cart.height(), back.height(), kEpsilon);
}

TEST(EllipsoidTest, RoundtripNegativeHeight) {
    // 地下 100m（海水面以下）
    auto cart = Cartographic::fromDegrees(0, 0, -100);
    const auto& e = Ellipsoid::WGS84();

    Vec3 ecef = e.cartographicToCartesian(cart);
    auto back = e.cartesianToCartographic(ecef);

    EXPECT_NEAR(cart.longitude(), back.longitude(), kEpsilon);
    EXPECT_NEAR(cart.latitude(), back.latitude(), kEpsilon);
    EXPECT_NEAR(cart.height(), back.height(), kEpsilon);
}

// ============================================================
// 赤道 & 本初子午线 — 已知参考值
// ============================================================

TEST(EllipsoidTest, EquatorPrimeMeridian) {
    auto cart = Cartographic::fromRadians(0, 0, 0);
    const auto& e = Ellipsoid::WGS84();
    Vec3 ecef = e.cartographicToCartesian(cart);

    EXPECT_NEAR(6378137.0, ecef.x(), 0.01);
    EXPECT_NEAR(0.0, ecef.y(), 0.01);
    EXPECT_NEAR(0.0, ecef.z(), 0.01);
}

TEST(EllipsoidTest, NorthPole) {
    // 北极，经度 0，高度 0
    auto cart = Cartographic::fromRadians(0, M_PI / 2.0, 0);
    const auto& e = Ellipsoid::WGS84();
    Vec3 ecef = e.cartographicToCartesian(cart);

    EXPECT_NEAR(0.0, ecef.x(), 0.01);
    EXPECT_NEAR(0.0, ecef.y(), 0.01);
    // z ≈ semi-minor axis (6356752.314245)
    EXPECT_NEAR(6356752.314245, ecef.z(), 0.01);
}

TEST(EllipsoidTest, East90Degrees) {
    // 赤道，东经 90°，高度 0
    auto cart = Cartographic::fromRadians(M_PI / 2.0, 0, 0);
    const auto& e = Ellipsoid::WGS84();
    Vec3 ecef = e.cartographicToCartesian(cart);

    EXPECT_NEAR(0.0, ecef.x(), 0.01);
    EXPECT_NEAR(6378137.0, ecef.y(), 0.01);
    EXPECT_NEAR(0.0, ecef.z(), 0.01);
}

// ============================================================
// 高纬度
// ============================================================

TEST(EllipsoidTest, HighLatitude85) {
    auto cart = Cartographic::fromDegrees(0, 85, 0);
    const auto& e = Ellipsoid::WGS84();
    Vec3 ecef = e.cartographicToCartesian(cart);
    auto back = e.cartesianToCartographic(ecef);

    EXPECT_NEAR(0, back.longitude(), kEpsilon);
    EXPECT_NEAR(85.0 * M_PI / 180.0, back.latitude(), kEpsilon);
    EXPECT_NEAR(0, back.height(), kEpsilon);
}

// ============================================================
// Degree / Radian 误用保护
// ============================================================

TEST(EllipsoidTest, DegreeVsRadian) {
    // 如果误把 degree 当 radian，ECEF 结果会完全不同
    auto radCart = Cartographic::fromRadians(1.0, 0.5, 0);   // ~57°, ~29°
    auto degCart = Cartographic::fromDegrees(1.0, 0.5, 0);   // ~1°, ~0.5°

    const auto& e = Ellipsoid::WGS84();
    Vec3 radEcef = e.cartographicToCartesian(radCart);
    Vec3 degEcef = e.cartographicToCartesian(degCart);

    // 结果应明显不同（距离 > 1000 km）
    double dist = radEcef.distanceTo(degEcef);
    EXPECT_GT(dist, 1000000.0);  // > 1000 km
}

TEST(TransformsTest, EcefToEnuMovesOriginToZero) {
    auto origin = Cartographic::fromDegrees(116.397, 39.908, 50.0);
    Vec3 originEcef = Ellipsoid::WGS84().cartographicToCartesian(origin);

    Vec3 enu = Transforms::ecefToEnu(origin) * originEcef;

    EXPECT_NEAR(0.0, enu.x(), 1e-6);
    EXPECT_NEAR(0.0, enu.y(), 1e-6);
    EXPECT_NEAR(0.0, enu.z(), 1e-6);
}

TEST(TransformsTest, EnuToEcefRoundtrip) {
    auto origin = Cartographic::fromDegrees(116.397, 39.908, 50.0);
    Vec3 localPoint(12.5, -30.0, 4.0);

    Mat4 enuToEcef = Transforms::enuToEcef(origin);
    Mat4 ecefToEnu = Transforms::ecefToEnu(origin);

    Vec3 back = ecefToEnu * (enuToEcef * localPoint);

    EXPECT_NEAR(localPoint.x(), back.x(), 1e-6);
    EXPECT_NEAR(localPoint.y(), back.y(), 1e-6);
    EXPECT_NEAR(localPoint.z(), back.z(), 1e-6);
}
