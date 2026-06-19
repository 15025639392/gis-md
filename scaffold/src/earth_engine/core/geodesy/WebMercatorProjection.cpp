#include "WebMercatorProjection.h"
#include "Ellipsoid.h"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace earth_engine {

WebMercatorProjection::WebMercatorProjection(const Ellipsoid& ellipsoid)
    : semimajorAxis_(ellipsoid.maximumRadius()),
      oneOverSemimajorAxis_(1.0 / ellipsoid.maximumRadius()) {}

Vec3 WebMercatorProjection::project(const Cartographic& cartographic) const {
    return Vec3(
        cartographic.longitude() * semimajorAxis_,
        geodeticLatitudeToMercatorAngle(cartographic.latitude()) *
            semimajorAxis_,
        cartographic.height());
}

Cartographic WebMercatorProjection::unproject(
    const Vec3& projectedCoordinates) const {
    return Cartographic(
        projectedCoordinates.x() * oneOverSemimajorAxis_,
        mercatorAngleToGeodeticLatitude(
            projectedCoordinates.y() * oneOverSemimajorAxis_),
        projectedCoordinates.z());
}

double WebMercatorProjection::maximumLatitude() {
    return mercatorAngleToGeodeticLatitude(glm::pi<double>());
}

double WebMercatorProjection::mercatorAngleToGeodeticLatitude(
    double mercatorAngle) {
    return glm::half_pi<double>() - 2.0 * std::atan(std::exp(-mercatorAngle));
}

double WebMercatorProjection::geodeticLatitudeToMercatorAngle(
    double latitude) {
    latitude = std::clamp(latitude, -maximumLatitude(), maximumLatitude());
    const double sinLatitude = std::sin(latitude);
    return 0.5 * std::log((1.0 + sinLatitude) / (1.0 - sinLatitude));
}

} // namespace earth_engine
