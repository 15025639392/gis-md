#include "RasterOverlayScreenSpaceMetrics.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"

#include <algorithm>

namespace earth_engine {
namespace {

bool signsDifferOrTouchesZero(double a, double b) {
    return a == 0.0 || b == 0.0 || (a < 0.0) != (b < 0.0);
}

double distanceOnEllipsoid(double lngA,
                           double latA,
                           double lngB,
                           double latB) {
    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 a = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(lngA, latA, 0.0));
    const Vec3 b = ellipsoid.cartographicToCartesian(
        Cartographic::fromRadians(lngB, latB, 0.0));
    return a.distanceTo(b);
}

RasterTargetScreenPixels computeProjectedRectangleSizeMeters(
    const Rectangle& bounds) {
    const double west = bounds.west();
    const double east = bounds.east();
    const double south = bounds.south();
    const double north = bounds.north();
    const double centerLng = west + bounds.width() * 0.5;

    const double lowerDistance = distanceOnEllipsoid(west, south, east, south);
    const double upperDistance = distanceOnEllipsoid(west, north, east, north);
    const double leftDistance = distanceOnEllipsoid(west, south, west, north);
    const double rightDistance = distanceOnEllipsoid(east, south, east, north);

    RasterTargetScreenPixels size;
    size.x = std::max(lowerDistance, upperDistance);
    size.y = std::max(leftDistance, rightDistance);

    // cesium-native Projection::computeProjectedRectangleSize: rectangles
    // spanning a large longitudinal range can have endpoints close together in
    // Cartesian space, so also check distances through the midpoint.
    const double halfDistanceLA =
        distanceOnEllipsoid(west, south, centerLng, south);
    const double halfDistanceLB =
        distanceOnEllipsoid(centerLng, south, east, south);
    const double halfDistanceUA =
        distanceOnEllipsoid(west, north, centerLng, north);
    const double halfDistanceUB =
        distanceOnEllipsoid(centerLng, north, east, north);
    if (halfDistanceLA > size.x || halfDistanceLB > size.x ||
        halfDistanceUA > size.x || halfDistanceUB > size.x) {
        size.x = std::max(halfDistanceLA + halfDistanceLB,
                          halfDistanceUA + halfDistanceUB);
    }

    if (signsDifferOrTouchesZero(west, east)) {
        size.y = std::max(
            size.y,
            distanceOnEllipsoid(0.0, south, 0.0, north));
    }

    if (signsDifferOrTouchesZero(south, north)) {
        const double equatorDistance =
            distanceOnEllipsoid(west, 0.0, east, 0.0);
        size.x = std::max(size.x, equatorDistance);

        const double halfDistanceL =
            distanceOnEllipsoid(west, 0.0, centerLng, 0.0);
        const double halfDistanceR =
            distanceOnEllipsoid(centerLng, 0.0, east, 0.0);
        if (halfDistanceL > size.x || halfDistanceR > size.x) {
            size.x = halfDistanceL + halfDistanceR;
        }
    }

    return size;
}

} // namespace

RasterTargetScreenPixels
RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
    const Rectangle& bounds,
    double geometricError,
    double maximumScreenSpaceError) {
    if (geometricError <= 0.0) return {};

    RasterTargetScreenPixels diameters =
        computeProjectedRectangleSizeMeters(bounds);
    diameters.x = std::max(
        1.0,
        diameters.x * maximumScreenSpaceError /
            geometricError);
    diameters.y = std::max(
        1.0,
        diameters.y * maximumScreenSpaceError /
            geometricError);
    return diameters;
}

RasterTargetScreenPixels
RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
    const Rectangle& bounds,
    RasterOverlayProjection projection,
    double geometricError,
    double maximumScreenSpaceError) {
    if (projection == RasterOverlayProjection::Geographic) {
        return computeDesiredScreenPixels(
            bounds,
            geometricError,
            maximumScreenSpaceError);
    }
    if (geometricError <= 0.0) return {};

    Projection projectionVariant =
        WebMercatorProjection(Ellipsoid::WGS84());
    const glm::dvec2 projectedSize =
        computeProjectedRectangleSize(
            projectionVariant,
            bounds,
            0.0,
            Ellipsoid::WGS84());
    return RasterTargetScreenPixels{
        std::max(1.0, projectedSize.x * maximumScreenSpaceError / geometricError),
        std::max(1.0, projectedSize.y * maximumScreenSpaceError / geometricError)};
}

} // namespace earth_engine
