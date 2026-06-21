#include <gtest/gtest.h>

#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/core/geodesy/Projection.h"
#include "earth_engine/core/math/Rectangle.h"
#include "earth_engine/tiling/RasterOverlayScreenSpaceMetrics.h"

#include <algorithm>

using namespace earth_engine;

namespace {

double surfaceDistance(double longitudeA,
                       double latitudeA,
                       double longitudeB,
                       double latitudeB) {
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const Vec3 a = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(longitudeA, latitudeA, 0.0));
    const Vec3 b = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(longitudeB, latitudeB, 0.0));
    return a.distanceTo(b);
}

TEST(RasterOverlayScreenSpaceMetricsTest,
     WebMercatorProjectedRectangleUnprojectsBeforeMeasuring) {
    const Rectangle geographicBounds =
        Rectangle::fromDegrees(-10.0, -5.0, -2.0, 5.0);
    const Rectangle webMercatorBounds = projectRectangleSimple(
        WebMercatorProjection(Ellipsoid::WGS84()),
        geographicBounds);
    constexpr double geometricError = 1024.0;
    constexpr double maximumScreenSpaceError = 8.0;

    const RasterTargetScreenPixels geographicPixels =
        RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
            geographicBounds,
            geometricError,
            maximumScreenSpaceError);
    const RasterTargetScreenPixels webMercatorPixels =
        RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
            webMercatorBounds,
            RasterOverlayProjection::WebMercator,
            geometricError,
            maximumScreenSpaceError);

    EXPECT_NEAR(geographicPixels.x, webMercatorPixels.x, 1e-6);
    EXPECT_NEAR(geographicPixels.y, webMercatorPixels.y, 1e-6);
}

} // namespace

TEST(RasterOverlayScreenSpaceMetricsTest,
     DesiredScreenPixelsMatchesCesiumNativeScaleFormula) {
    // cesium-native RasterOverlayUtilities::computeDesiredScreenPixels:
    // projected rectangle meters * maximumScreenSpaceError / geometricError.
    const Rectangle bounds = Rectangle::fromDegrees(-1.0, 0.0, 1.0, 1.0);
    constexpr double geometricError = 512.0;
    constexpr double maximumScreenSpaceError = 16.0;

    const RasterTargetScreenPixels pixels =
        RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
            bounds,
            geometricError,
            maximumScreenSpaceError);

    const double lowerWidth = surfaceDistance(
        bounds.west(),
        bounds.south(),
        bounds.east(),
        bounds.south());
    const double upperWidth = surfaceDistance(
        bounds.west(),
        bounds.north(),
        bounds.east(),
        bounds.north());
    const double height = surfaceDistance(
        0.0,
        bounds.south(),
        0.0,
        bounds.north());
    const double scale = maximumScreenSpaceError / geometricError;

    EXPECT_NEAR(std::max(lowerWidth, upperWidth) * scale, pixels.x, 1e-6);
    EXPECT_NEAR(height * scale, pixels.y, 1e-6);
}
