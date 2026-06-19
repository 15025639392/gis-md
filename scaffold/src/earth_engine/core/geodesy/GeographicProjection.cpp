#include "GeographicProjection.h"
#include "Ellipsoid.h"

namespace earth_engine {

GeographicProjection::GeographicProjection(const Ellipsoid& ellipsoid)
    : semimajorAxis_(ellipsoid.maximumRadius()),
      oneOverSemimajorAxis_(1.0 / ellipsoid.maximumRadius()) {}

Vec3 GeographicProjection::project(const Cartographic& cartographic) const {
    return Vec3(cartographic.longitude() * semimajorAxis_,
                cartographic.latitude() * semimajorAxis_,
                cartographic.height());
}

Cartographic GeographicProjection::unproject(
    const Vec3& projectedCoordinates) const {
    return Cartographic(projectedCoordinates.x() * oneOverSemimajorAxis_,
                        projectedCoordinates.y() * oneOverSemimajorAxis_,
                        projectedCoordinates.z());
}

} // namespace earth_engine
