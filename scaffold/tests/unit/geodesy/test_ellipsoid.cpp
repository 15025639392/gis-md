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
    EXPECT_DOUBLE_EQ(6356752.3142451793, e.semiMinorAxis());
    EXPECT_NEAR(1.0 / 298.257223563, e.flattening(), 1e-12);
}

TEST(EllipsoidTest, UnitSphereConstantMatchesCesiumNative) {
    // cesium-native exposes Ellipsoid::UNIT_SPHERE as radii (1, 1, 1).
    const auto& e = Ellipsoid::UNIT_SPHERE();

    EXPECT_EQ(Vec3(1.0, 1.0, 1.0), e.radii());
    EXPECT_DOUBLE_EQ(1.0, e.semiMajorAxis());
    EXPECT_DOUBLE_EQ(1.0, e.semiMinorAxis());
    EXPECT_DOUBLE_EQ(1.0, e.maximumRadius());
    EXPECT_DOUBLE_EQ(1.0, e.minimumRadius());
    EXPECT_DOUBLE_EQ(0.0, e.flattening());
    EXPECT_EQ(Vec3(1.0, 0.0, 0.0),
              e.cartographicToCartesian(Cartographic::fromRadians(0.0, 0.0)));
}

TEST(EllipsoidTest, TriAxialRadiiStateMatchesCesiumNative) {
    // cesium-native Ellipsoid stores independent x/y/z radii and derives
    // squared and reciprocal state from all three components.
    const Ellipsoid e(2.0, 3.0, 4.0);

    EXPECT_EQ(Vec3(2.0, 3.0, 4.0), e.radii());
    EXPECT_DOUBLE_EQ(4.0, e.maximumRadius());
    EXPECT_DOUBLE_EQ(2.0, e.minimumRadius());
}

TEST(EllipsoidTest, EqualityUsesCesiumNativeRadiiSemantics) {
    const Ellipsoid lhs(1.0, 2.0, 3.0);
    const Ellipsoid same(1.0, 2.0, 3.0);
    const Ellipsoid different(1.0, 2.0, 4.0);

    EXPECT_TRUE(lhs == same);
    EXPECT_FALSE(lhs != same);
    EXPECT_FALSE(lhs == different);
    EXPECT_TRUE(lhs != different);
}

TEST(EllipsoidTest, TriAxialCartographicToCartesianUsesAllRadii) {
    const Ellipsoid e(2.0, 3.0, 4.0);

    Vec3 xEquator = e.cartographicToCartesian(
        Cartographic::fromRadians(0.0, 0.0, 0.0));
    EXPECT_NEAR(2.0, xEquator.x(), 1e-12);
    EXPECT_NEAR(0.0, xEquator.y(), 1e-12);
    EXPECT_NEAR(0.0, xEquator.z(), 1e-12);

    Vec3 yEquator = e.cartographicToCartesian(
        Cartographic::fromRadians(M_PI / 2.0, 0.0, 0.0));
    EXPECT_NEAR(0.0, yEquator.x(), 1e-12);
    EXPECT_NEAR(3.0, yEquator.y(), 1e-12);
    EXPECT_NEAR(0.0, yEquator.z(), 1e-12);
}

TEST(EllipsoidTest, CartesianToCartographicPreservesCesiumNativeNegativeHeight) {
    // cesium-native computes height as sign(dot(cartesian - surface,
    // cartesian)) * length(cartesian - surface), so points below the surface
    // preserve negative height.
    const Ellipsoid e(2.0, 3.0, 4.0);
    const Cartographic cartographic =
        Cartographic::fromRadians(0.4, -0.3, -0.25);

    const Vec3 cartesian = e.cartographicToCartesian(cartographic);
    const std::optional<Cartographic> roundtrip =
        e.tryCartesianToCartographic(cartesian);

    ASSERT_TRUE(roundtrip.has_value());
    EXPECT_NEAR(cartographic.longitude(), roundtrip->longitude(), 1e-12);
    EXPECT_NEAR(cartographic.latitude(), roundtrip->latitude(), 1e-12);
    EXPECT_NEAR(cartographic.height(), roundtrip->height(), 1e-12);
}

TEST(EllipsoidTest, TriAxialGeodeticSurfaceNormalUsesAllRadii) {
    const Ellipsoid e(2.0, 3.0, 4.0);

    Vec3 normal = e.geodeticSurfaceNormal(Vec3(2.0, 3.0, 0.0));
    const Vec3 expected = Vec3(0.5, 1.0 / 3.0, 0.0).normalized();

    EXPECT_NEAR(expected.x(), normal.x(), 1e-12);
    EXPECT_NEAR(expected.y(), normal.y(), 1e-12);
    EXPECT_NEAR(expected.z(), normal.z(), 1e-12);
}

TEST(EllipsoidTest, GeodeticSurfaceNormalAtCenterMatchesCesiumNativeNonFinite) {
    // Cesium-native normalizes position * oneOverRadiiSquared directly; the
    // ellipsoid center is not a valid geodetic surface-normal input.
    const Vec3 normal = Ellipsoid::WGS84().geodeticSurfaceNormal(Vec3::zero());

    EXPECT_FALSE(std::isfinite(normal.x()));
    EXPECT_FALSE(std::isfinite(normal.y()));
    EXPECT_FALSE(std::isfinite(normal.z()));
}

TEST(EllipsoidTest, TriAxialRayIntersectionIntervalUsesAllRadii) {
    const Ellipsoid e(2.0, 3.0, 4.0);

    auto interval = e.rayIntersectionInterval(Vec3(0.0, 6.0, 0.0),
                                              Vec3(0.0, -1.0, 0.0));
    ASSERT_TRUE(interval.has_value());
    EXPECT_NEAR(3.0, interval->entryDistance, 1e-12);
    EXPECT_NEAR(9.0, interval->exitDistance, 1e-12);
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
    // z ≈ semi-minor axis (6356752.3142451793)
    EXPECT_NEAR(6356752.3142451793, ecef.z(), 0.01);
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

TEST(EllipsoidTest, ProjectToSurfaceMatchesEllipsoidHeightZero) {
    const auto& e = Ellipsoid::WGS84();
    auto cart = Cartographic::fromDegrees(116.397, 39.908, 1200.0);
    Vec3 ecef = e.cartographicToCartesian(cart);

    Vec3 surface = e.projectToSurface(ecef);
    Cartographic back = e.cartesianToCartographic(surface);

    EXPECT_NEAR(cart.longitude(), back.longitude(), 1e-10);
    EXPECT_NEAR(cart.latitude(), back.latitude(), 1e-10);
    EXPECT_NEAR(0.0, back.height(), 1e-5);
}

TEST(EllipsoidTest, TryScaleToGeodeticSurfaceMatchesCesiumNativeCenterBehavior) {
    const auto& e = Ellipsoid::WGS84();

    EXPECT_FALSE(e.tryScaleToGeodeticSurface(Vec3::zero()).has_value());
    EXPECT_FALSE(e.tryCartesianToCartographic(Vec3::zero()).has_value());

    auto nearCenter = e.tryScaleToGeodeticSurface(Vec3(1.0, 0.0, 0.0));
    ASSERT_TRUE(nearCenter.has_value());
    EXPECT_NEAR(e.semiMajorAxis(), nearCenter->x(), 1e-6);
    EXPECT_NEAR(0.0, nearCenter->y(), 1e-12);
    EXPECT_NEAR(0.0, nearCenter->z(), 1e-12);
}

TEST(EllipsoidTest, TryScaleToGeocentricSurfaceMatchesCesiumNative) {
    const Ellipsoid e(2.0, 3.0, 4.0);

    EXPECT_FALSE(e.tryScaleToGeocentricSurface(Vec3::zero()).has_value());

    auto xAxis = e.tryScaleToGeocentricSurface(Vec3(8.0, 0.0, 0.0));
    ASSERT_TRUE(xAxis.has_value());
    EXPECT_NEAR(2.0, xAxis->x(), 1e-12);
    EXPECT_NEAR(0.0, xAxis->y(), 1e-12);
    EXPECT_NEAR(0.0, xAxis->z(), 1e-12);

    auto diagonal = e.tryScaleToGeocentricSurface(Vec3(2.0, 3.0, 4.0));
    ASSERT_TRUE(diagonal.has_value());
    const double beta = 1.0 / std::sqrt(3.0);
    EXPECT_NEAR(2.0 * beta, diagonal->x(), 1e-12);
    EXPECT_NEAR(3.0 * beta, diagonal->y(), 1e-12);
    EXPECT_NEAR(4.0 * beta, diagonal->z(), 1e-12);
}

TEST(EllipsoidTest, RayIntersectionHasExplicitMissAndHit) {
    const auto& e = Ellipsoid::WGS84();
    Vec3 origin(0.0, 0.0, 7000000.0);
    Vec3 inward(0.0, 0.0, -1.0);
    Vec3 outward(0.0, 0.0, 1.0);

    auto hit = e.rayIntersection(origin, inward);
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(e.semiMinorAxis(), hit->z(), 1e-3);

    auto miss = e.rayIntersection(origin, outward);
    EXPECT_FALSE(miss.has_value());
}

TEST(EllipsoidTest, RayIntersectionIntervalMatchesCesiumNative) {
    const Ellipsoid unitSphere(1.0, 1.0);

    auto outside = unitSphere.rayIntersectionInterval(
        Vec3(2.0, 0.0, 0.0),
        Vec3(-1.0, 0.0, 0.0));
    ASSERT_TRUE(outside.has_value());
    EXPECT_NEAR(1.0, outside->entryDistance, 1e-12);
    EXPECT_NEAR(3.0, outside->exitDistance, 1e-12);

    auto inside = Ellipsoid::WGS84().rayIntersectionInterval(
        Vec3(20000.0, 0.0, 0.0),
        Vec3(1.0, 0.0, 0.0));
    ASSERT_TRUE(inside.has_value());
    EXPECT_DOUBLE_EQ(0.0, inside->entryDistance);
    EXPECT_NEAR(Ellipsoid::WGS84().semiMajorAxis() - 20000.0,
                inside->exitDistance,
                1e-6);

    EXPECT_FALSE(unitSphere
                     .rayIntersectionInterval(Vec3(1.0, 0.0, 0.0),
                                              Vec3(0.0, 0.0, 1.0))
                     .has_value());
}

TEST(EllipsoidTest, RayIntersectionIntervalMatchesCesiumNativeAxisCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp:
    // IntersectionTests::rayEllipsoid outside intersections.
    const Ellipsoid unitSphere(1.0, 1.0);
    struct Case {
        Vec3 origin;
        Vec3 direction;
    };
    const Case cases[] = {
        {Vec3(2.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)},
        {Vec3(0.0, 2.0, 0.0), Vec3(0.0, -1.0, 0.0)},
        {Vec3(0.0, 0.0, 2.0), Vec3(0.0, 0.0, -1.0)},
        {Vec3(-2.0, 0.0, 0.0), Vec3(1.0, 0.0, 0.0)},
        {Vec3(0.0, -2.0, 0.0), Vec3(0.0, 1.0, 0.0)},
        {Vec3(0.0, 0.0, -2.0), Vec3(0.0, 0.0, 1.0)},
    };

    for (const auto& c : cases) {
        auto interval = unitSphere.rayIntersectionInterval(c.origin, c.direction);
        ASSERT_TRUE(interval.has_value());
        EXPECT_NEAR(1.0, interval->entryDistance, 1e-12);
        EXPECT_NEAR(3.0, interval->exitDistance, 1e-12);
    }
}

TEST(EllipsoidTest, RayIntersectionIntervalMatchesCesiumNativeMissCases) {
    // Ported from cesium-native CesiumGeometry/test/TestIntersectionTests.cpp:
    // rays outside pointing away, tangent rays, and parallel miss cases.
    const Ellipsoid unitSphere(1.0, 1.0);
    struct Case {
        Vec3 origin;
        Vec3 direction;
    };
    const Case cases[] = {
        {Vec3(-2.0, 0.0, 0.0), Vec3(-1.0, 0.0, 0.0)},
        {Vec3(0.0, -2.0, 0.0), Vec3(0.0, -1.0, 0.0)},
        {Vec3(0.0, 0.0, -2.0), Vec3(0.0, 0.0, -1.0)},
        {Vec3(1.0, 0.0, 0.0), Vec3(0.0, 0.0, 1.0)},
        {Vec3(2.0, 0.0, 0.0), Vec3(0.0, 0.0, 1.0)},
        {Vec3(2.0, 0.0, 0.0), Vec3(0.0, 0.0, -1.0)},
        {Vec3(2.0, 0.0, 0.0), Vec3(0.0, 1.0, 0.0)},
        {Vec3(2.0, 0.0, 0.0), Vec3(0.0, -1.0, 0.0)},
    };

    for (const auto& c : cases) {
        EXPECT_FALSE(unitSphere.rayIntersectionInterval(c.origin, c.direction)
                         .has_value());
    }
}

TEST(EllipsoidTest, VincentyInverseBeijingShanghaiDistance) {
    const auto& e = Ellipsoid::WGS84();
    auto beijing = Cartographic::fromDegrees(116.397, 39.908, 0.0);
    auto shanghai = Cartographic::fromDegrees(121.4737, 31.2304, 0.0);

    GeodesicInverseResult inv = e.inverse(beijing, shanghai);

    EXPECT_TRUE(inv.converged);
    EXPECT_NEAR(1066626.6, inv.distanceMeters, 1.0);
    EXPECT_GE(inv.initialAzimuthRadians, 0.0);
    EXPECT_LT(inv.initialAzimuthRadians, 2.0 * M_PI);
}

TEST(EllipsoidTest, VincentyDirectRoundtripsInverse) {
    const auto& e = Ellipsoid::WGS84();
    auto start = Cartographic::fromDegrees(116.397, 39.908, 0.0);
    auto end = Cartographic::fromDegrees(121.4737, 31.2304, 0.0);

    GeodesicInverseResult inv = e.inverse(start, end);
    ASSERT_TRUE(inv.converged);
    GeodesicDirectResult direct = e.direct(start, inv.initialAzimuthRadians,
                                           inv.distanceMeters);

    EXPECT_TRUE(direct.converged);
    EXPECT_NEAR(end.longitude(), direct.destination.longitude(), 1e-8);
    EXPECT_NEAR(end.latitude(), direct.destination.latitude(), 1e-8);
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

TEST(TransformsTest, EastNorthUpToFixedFrameMatchesCesiumNativeSpecialCases) {
    auto columnMatches = [](const Mat4& m,
                            int col,
                            const Vec3& expected,
                            double epsilon = 1e-12) {
        return std::abs(m(0, col) - expected.x()) < epsilon &&
               std::abs(m(1, col) - expected.y()) < epsilon &&
               std::abs(m(2, col) - expected.z()) < epsilon;
    };

    Mat4 zeroFrame = Transforms::eastNorthUpToFixedFrame(Vec3::zero());
    EXPECT_TRUE(columnMatches(zeroFrame, 0, Vec3(0.0, 1.0, 0.0)));
    EXPECT_TRUE(columnMatches(zeroFrame, 1, Vec3(-1.0, 0.0, 0.0)));
    EXPECT_TRUE(columnMatches(zeroFrame, 2, Vec3(0.0, 0.0, 1.0)));

    double polarRadius = Ellipsoid::WGS84().semiMinorAxis();
    Mat4 southPoleFrame =
        Transforms::eastNorthUpToFixedFrame(Vec3(0.0, 0.0, -polarRadius));
    EXPECT_TRUE(columnMatches(southPoleFrame, 0, Vec3(0.0, 1.0, 0.0)));
    EXPECT_TRUE(columnMatches(southPoleFrame, 1, Vec3(1.0, 0.0, 0.0)));
    EXPECT_TRUE(columnMatches(southPoleFrame, 2, Vec3(0.0, 0.0, -1.0)));
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
