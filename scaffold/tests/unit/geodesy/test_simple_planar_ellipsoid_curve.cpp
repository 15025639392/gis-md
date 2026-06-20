#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/SimplePlanarEllipsoidCurve.h"
#include "earth_engine/core/math/Vec3.h"

using namespace earth_engine;

namespace {

const Vec3 kPhiladelphiaEcef(
    1253264.69280105,
    -4732469.91065521,
    4075112.40412297);
const Vec3 kTokyoEcef(
    -3960158.65587452,
    3352568.87555906,
    3697235.23506459);
const Vec3 kPhiladelphiaAntipodeEcef(
    -1253369.920224856,
    4732412.7444064,
    -4075146.2160252854);

const Cartographic kPhiladelphiaLlh(
    -1.3119164210487293,
    0.6974930673711344,
    373.64791900173714);
const Cartographic kTokyoLlh(
    2.4390907007049445,
    0.6222806863437318,
    283.242432000711);

const Vec3 kNewYorkCityEcef(
    1329752.6826922249,
    -4657851.870887691,
    4140135.1399898543);
const Vec3 kTimesSquareEcef(
    1334771.9227395034,
    -4650343.070699833,
    4142168.965635141);

void expectVec3Near(const Vec3& expected, const Vec3& actual, double epsilon) {
    EXPECT_NEAR(expected.x(), actual.x(), epsilon);
    EXPECT_NEAR(expected.y(), actual.y(), epsilon);
    EXPECT_NEAR(expected.z(), actual.z(), epsilon);
}

} // namespace

TEST(SimplePlanarEllipsoidCurveTest, EndpointsMatchInputEcefCoordinates) {
    const std::optional<SimplePlanarEllipsoidCurve> curve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kPhiladelphiaEcef,
            kTokyoEcef);

    ASSERT_TRUE(curve.has_value());
    expectVec3Near(kPhiladelphiaEcef, curve->getPosition(0.0), 1e-6);
    expectVec3Near(kTokyoEcef, curve->getPosition(1.0), 1e-6);
}

TEST(SimplePlanarEllipsoidCurveTest, EndpointBranchesClampToOriginalEcefCoordinates) {
    const std::optional<SimplePlanarEllipsoidCurve> curve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kPhiladelphiaEcef,
            kTokyoEcef);

    ASSERT_TRUE(curve.has_value());
    expectVec3Near(kPhiladelphiaEcef, curve->getPosition(-0.25, 1000.0), 1e-6);
    expectVec3Near(kPhiladelphiaEcef, curve->getPosition(0.0, 1000.0), 1e-6);
    expectVec3Near(kTokyoEcef, curve->getPosition(1.0, 1000.0), 1e-6);
    expectVec3Near(kTokyoEcef, curve->getPosition(1.25, 1000.0), 1e-6);
}

TEST(SimplePlanarEllipsoidCurveTest, MidpointIsCoplanarWithEndpointsAndEarthCenter) {
    const std::optional<SimplePlanarEllipsoidCurve> curve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kPhiladelphiaEcef,
            kTokyoEcef);

    ASSERT_TRUE(curve.has_value());
    const Vec3 midpoint = curve->getPosition(0.5);
    const Vec3 planeNormal =
        (kPhiladelphiaEcef - midpoint).cross(kTokyoEcef - midpoint).normalized();

    EXPECT_NEAR(0.0, midpoint.dot(planeNormal), 1e-5);
}

TEST(SimplePlanarEllipsoidCurveTest, RejectsCenterEcefCoordinates) {
    const std::optional<SimplePlanarEllipsoidCurve> curve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kPhiladelphiaEcef,
            Vec3::zero());

    EXPECT_FALSE(curve.has_value());
}

TEST(SimplePlanarEllipsoidCurveTest, SameStartAndEndPointStaysFixed) {
    const std::optional<SimplePlanarEllipsoidCurve> curve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kPhiladelphiaEcef,
            kPhiladelphiaEcef);

    ASSERT_TRUE(curve.has_value());
    expectVec3Near(kPhiladelphiaEcef, curve->getPosition(0.25), 1e-6);
    expectVec3Near(kPhiladelphiaEcef, curve->getPosition(0.5), 1e-6);
    expectVec3Near(kPhiladelphiaEcef, curve->getPosition(0.75), 1e-6);
}

TEST(SimplePlanarEllipsoidCurveTest, InterpolatesHeightAndAdditionalHeight) {
    const double startHeight = 100.0;
    const double endHeight = 25.0;
    const std::optional<SimplePlanarEllipsoidCurve> curve =
        SimplePlanarEllipsoidCurve::fromLongitudeLatitudeHeight(
            Ellipsoid::WGS84(),
            Cartographic(0.25, 1.0, startHeight),
            Cartographic(0.25, 1.0, endHeight));

    ASSERT_TRUE(curve.has_value());
    const Vec3 midpoint = curve->getPosition(0.5, 10.0);
    const std::optional<Cartographic> cartographic =
        Ellipsoid::WGS84().tryCartesianToCartographic(midpoint);

    ASSERT_TRUE(cartographic.has_value());
    EXPECT_NEAR(72.5, cartographic->height(), 1e-4);
}

TEST(SimplePlanarEllipsoidCurveTest, HandlesNegativeHeightPathWithoutFlippingEarthSide) {
    const std::optional<SimplePlanarEllipsoidCurve> curve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kTimesSquareEcef,
            kNewYorkCityEcef);

    ASSERT_TRUE(curve.has_value());
    const Vec3 midpoint = curve->getPosition(0.5);
    const double expectedDistance =
        kTimesSquareEcef.distanceTo(kNewYorkCityEcef);
    const double actualDistance =
        kTimesSquareEcef.distanceTo(midpoint) +
        kNewYorkCityEcef.distanceTo(midpoint);

    EXPECT_NEAR(expectedDistance, actualDistance, 1e-3);
}

TEST(SimplePlanarEllipsoidCurveTest, NearAntipodePathStaysAboveEarth) {
    const std::optional<SimplePlanarEllipsoidCurve> curve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kPhiladelphiaEcef,
            kPhiladelphiaAntipodeEcef);

    ASSERT_TRUE(curve.has_value());
    for (const double percentage : {0.25, 0.5, 0.75}) {
        const std::optional<Cartographic> cartographic =
            Ellipsoid::WGS84().tryCartesianToCartographic(
                curve->getPosition(percentage));

        ASSERT_TRUE(cartographic.has_value());
        EXPECT_GT(cartographic->height(), 0.0);
    }
}

TEST(SimplePlanarEllipsoidCurveTest, ReversePathHasSameMidpoint) {
    const std::optional<SimplePlanarEllipsoidCurve> forwardCurve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kPhiladelphiaEcef,
            kTokyoEcef);
    const std::optional<SimplePlanarEllipsoidCurve> reverseCurve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kTokyoEcef,
            kPhiladelphiaEcef);

    ASSERT_TRUE(forwardCurve.has_value());
    ASSERT_TRUE(reverseCurve.has_value());
    expectVec3Near(forwardCurve->getPosition(0.5),
                   reverseCurve->getPosition(0.5),
                   1e-6);
}

TEST(SimplePlanarEllipsoidCurveTest, LlhConstructorMatchesEquivalentEcefCurve) {
    const std::optional<SimplePlanarEllipsoidCurve> llhCurve =
        SimplePlanarEllipsoidCurve::fromLongitudeLatitudeHeight(
            Ellipsoid::WGS84(),
            kPhiladelphiaLlh,
            kTokyoLlh);
    const std::optional<SimplePlanarEllipsoidCurve> ecefCurve =
        SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
            Ellipsoid::WGS84(),
            kPhiladelphiaEcef,
            kTokyoEcef);

    ASSERT_TRUE(llhCurve.has_value());
    ASSERT_TRUE(ecefCurve.has_value());
    expectVec3Near(ecefCurve->getPosition(0.5),
                   llhCurve->getPosition(0.5),
                   1e-6);
}
