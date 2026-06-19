#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/GeographicProjection.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/geodesy/WebMercatorProjection.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/core/math/Vec3.h"

#include <cmath>

using namespace earth_engine;

TEST(GeographicProjectionTest, ProjectUsesCesiumNativeLinearRadiansScale) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Cartographic input =
        Cartographic::fromRadians(1.0, 0.5, 1234.5);

    const Vec3 projected = projection.project(input);

    EXPECT_DOUBLE_EQ(Ellipsoid::WGS84().maximumRadius(), projected.x());
    EXPECT_DOUBLE_EQ(Ellipsoid::WGS84().maximumRadius() * 0.5, projected.y());
    EXPECT_DOUBLE_EQ(1234.5, projected.z());
}

TEST(GeographicProjectionTest, UnprojectPreservesHeight) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Vec3 projected(Ellipsoid::WGS84().maximumRadius(),
                         Ellipsoid::WGS84().maximumRadius() * -0.25,
                         987.0);

    const Cartographic cartographic = projection.unproject(projected);

    EXPECT_DOUBLE_EQ(1.0, cartographic.longitude());
    EXPECT_DOUBLE_EQ(-0.25, cartographic.latitude());
    EXPECT_DOUBLE_EQ(987.0, cartographic.height());
}

TEST(GeographicProjectionTest, ProjectAndUnprojectRectangleMatchesCesiumNative) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Rectangle globeRectangle =
        Rectangle::fromDegrees(-120.0, -40.0, -60.0, 30.0);

    const Rectangle projected = projection.project(globeRectangle);
    EXPECT_NEAR(globeRectangle.west() * Ellipsoid::WGS84().maximumRadius(),
                projected.west(),
                1e-9);
    EXPECT_NEAR(globeRectangle.south() * Ellipsoid::WGS84().maximumRadius(),
                projected.south(),
                1e-9);
    EXPECT_NEAR(globeRectangle.east() * Ellipsoid::WGS84().maximumRadius(),
                projected.east(),
                1e-9);
    EXPECT_NEAR(globeRectangle.north() * Ellipsoid::WGS84().maximumRadius(),
                projected.north(),
                1e-9);

    const Rectangle unprojected = projection.unproject(projected);
    EXPECT_NEAR(globeRectangle.west(), unprojected.west(), 1e-15);
    EXPECT_NEAR(globeRectangle.south(), unprojected.south(), 1e-15);
    EXPECT_NEAR(globeRectangle.east(), unprojected.east(), 1e-15);
    EXPECT_NEAR(globeRectangle.north(), unprojected.north(), 1e-15);
}

TEST(GeographicProjectionTest, MaximumProjectedRectangleMatchesCesiumNative) {
    const Rectangle maximum =
        GeographicProjection::computeMaximumProjectedRectangle(Ellipsoid::WGS84());

    EXPECT_NEAR(-M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.west(),
                1e-9);
    EXPECT_NEAR(-M_PI * 0.5 * Ellipsoid::WGS84().maximumRadius(),
                maximum.south(),
                1e-9);
    EXPECT_NEAR(M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.east(),
                1e-9);
    EXPECT_NEAR(M_PI * 0.5 * Ellipsoid::WGS84().maximumRadius(),
                maximum.north(),
                1e-9);
}

TEST(GeographicProjectionTest, EqualityUsesCesiumNativeEllipsoidSemantics) {
    const GeographicProjection lhs(Ellipsoid::WGS84());
    const GeographicProjection same(
        Ellipsoid(6378137.0, 6378137.0, 6356752.3142451793));
    const GeographicProjection different(Ellipsoid::UNIT_SPHERE());

    EXPECT_TRUE(lhs == same);
    EXPECT_FALSE(lhs != same);
    EXPECT_FALSE(lhs == different);
    EXPECT_TRUE(lhs != different);
}

TEST(WebMercatorProjectionTest, MaximumLatitudeMatchesCesiumNative) {
    EXPECT_NEAR(1.4844222297453324,
                WebMercatorProjection::maximumLatitude(),
                1e-15);
    EXPECT_NEAR(M_PI,
                WebMercatorProjection::geodeticLatitudeToMercatorAngle(
                    WebMercatorProjection::maximumLatitude()),
                1e-14);
}

TEST(WebMercatorProjectionTest, MercatorAngleRoundtripsLatitude) {
    const double latitude = 40.0 * M_PI / 180.0;

    const double mercator =
        WebMercatorProjection::geodeticLatitudeToMercatorAngle(latitude);
    const double roundtrip =
        WebMercatorProjection::mercatorAngleToGeodeticLatitude(mercator);

    EXPECT_NEAR(latitude, roundtrip, 1e-14);
}

TEST(WebMercatorProjectionTest, GeodeticLatitudeClampsToValidMercatorRange) {
    EXPECT_NEAR(M_PI,
                WebMercatorProjection::geodeticLatitudeToMercatorAngle(
                    M_PI / 2.0),
                1e-14);
    EXPECT_NEAR(-M_PI,
                WebMercatorProjection::geodeticLatitudeToMercatorAngle(
                    -M_PI / 2.0),
                1e-14);
}

TEST(WebMercatorProjectionTest, ProjectAndUnprojectPreserveHeight) {
    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Cartographic input =
        Cartographic::fromDegrees(116.397, 39.908, 1234.5);

    const Vec3 projected = projection.project(input);
    const Cartographic roundtrip = projection.unproject(projected);

    EXPECT_NEAR(input.longitude(), roundtrip.longitude(), 1e-14);
    EXPECT_NEAR(input.latitude(), roundtrip.latitude(), 1e-14);
    EXPECT_DOUBLE_EQ(input.height(), roundtrip.height());
}

TEST(WebMercatorProjectionTest, ProjectAndUnprojectRectangleMatchesCesiumNative) {
    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Rectangle globeRectangle =
        Rectangle::fromDegrees(-120.0, -40.0, -60.0, 30.0);

    const Rectangle projected = projection.project(globeRectangle);
    const Vec3 southwest =
        projection.project(Cartographic(globeRectangle.west(), globeRectangle.south(), 0.0));
    const Vec3 northeast =
        projection.project(Cartographic(globeRectangle.east(), globeRectangle.north(), 0.0));
    EXPECT_NEAR(southwest.x(), projected.west(), 1e-9);
    EXPECT_NEAR(southwest.y(), projected.south(), 1e-9);
    EXPECT_NEAR(northeast.x(), projected.east(), 1e-9);
    EXPECT_NEAR(northeast.y(), projected.north(), 1e-9);

    const Rectangle unprojected = projection.unproject(projected);
    EXPECT_NEAR(globeRectangle.west(), unprojected.west(), 1e-15);
    EXPECT_NEAR(globeRectangle.south(), unprojected.south(), 1e-14);
    EXPECT_NEAR(globeRectangle.east(), unprojected.east(), 1e-15);
    EXPECT_NEAR(globeRectangle.north(), unprojected.north(), 1e-14);
}

TEST(WebMercatorProjectionTest, MaximumProjectedRectangleMatchesCesiumNative) {
    const Rectangle maximum =
        WebMercatorProjection::computeMaximumProjectedRectangle(Ellipsoid::WGS84());

    EXPECT_NEAR(-M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.west(),
                1e-9);
    EXPECT_NEAR(-M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.south(),
                1e-9);
    EXPECT_NEAR(M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.east(),
                1e-9);
    EXPECT_NEAR(M_PI * Ellipsoid::WGS84().maximumRadius(),
                maximum.north(),
                1e-9);
}

TEST(WebMercatorProjectionTest, EqualityUsesCesiumNativeEllipsoidSemantics) {
    const WebMercatorProjection lhs(Ellipsoid::WGS84());
    const WebMercatorProjection same(
        Ellipsoid(6378137.0, 6378137.0, 6356752.3142451793));
    const WebMercatorProjection different(Ellipsoid::UNIT_SPHERE());

    EXPECT_TRUE(lhs == same);
    EXPECT_FALSE(lhs != same);
    EXPECT_FALSE(lhs == different);
    EXPECT_TRUE(lhs != different);
}

TEST(ProjectionTest, ComputeProjectedRectangleSizeMatchesCesiumNativeGlobeCases) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    double maxHeight = 0.0;

    Rectangle entireGlobe = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-180.0, -90.0, 180.0, 90.0));
    glm::dvec2 entireSize = computeProjectedRectangleSize(
        projection,
        entireGlobe,
        maxHeight,
        ellipsoid);
    EXPECT_GT(entireSize.x, ellipsoid.maximumRadius() * 2.0);
    EXPECT_NEAR(ellipsoid.minimumRadius() * 2.0, entireSize.y, 1.0);

    Rectangle westernHemisphere = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-180.0, -90.0, 0.0, 90.0));
    glm::dvec2 westernSize = computeProjectedRectangleSize(
        projection,
        westernHemisphere,
        maxHeight,
        ellipsoid);
    EXPECT_NEAR(ellipsoid.maximumRadius() * 2.0, westernSize.x, 1.0);
    EXPECT_NEAR(ellipsoid.minimumRadius() * 2.0, westernSize.y, 1.0);

    Rectangle easternHemisphere = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(0.0, -90.0, 180.0, 90.0));
    glm::dvec2 easternSize = computeProjectedRectangleSize(
        projection,
        easternHemisphere,
        maxHeight,
        ellipsoid);
    EXPECT_NEAR(ellipsoid.maximumRadius() * 2.0, easternSize.x, 1.0);
    EXPECT_NEAR(ellipsoid.minimumRadius() * 2.0, easternSize.y, 1.0);
}

TEST(ProjectionTest, ComputeProjectedRectangleSizeMatchesCesiumNativeEquatorCases) {
    const GeographicProjection projection(Ellipsoid::WGS84());
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    double maxHeight = 0.0;

    Rectangle crossingEquator = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-100.0, -70.0, -80.0, 40.0));
    glm::dvec2 crossingSize = computeProjectedRectangleSize(
        projection,
        crossingEquator,
        maxHeight,
        ellipsoid);
    const double equatorWidth = ellipsoid
        .cartographicToCartesian(Cartographic::fromDegrees(-100.0, 0.0, 0.0))
        .distanceTo(ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(-80.0, 0.0, 0.0)));
    EXPECT_NEAR(equatorWidth, crossingSize.x, 1.0);

    Rectangle narrowBand = projectRectangleSimple(
        projection,
        Rectangle::fromDegrees(-180.0, 20.0, 180.0, 40.0));
    glm::dvec2 narrowSize = computeProjectedRectangleSize(
        projection,
        narrowBand,
        maxHeight,
        ellipsoid);
    const double meridianHeight = ellipsoid
        .cartographicToCartesian(Cartographic::fromDegrees(0.0, 20.0, 0.0))
        .distanceTo(ellipsoid.cartographicToCartesian(
            Cartographic::fromDegrees(0.0, 40.0, 0.0)));
    EXPECT_GT(narrowSize.x, ellipsoid.maximumRadius() * 2.0);
    EXPECT_NEAR(meridianHeight, narrowSize.y, 1.0);
}
