#include "GeographicProjection.h"
#include "Ellipsoid.h"
#include "../math/MathUtils.h"

namespace earth_engine {

GeographicProjection::GeographicProjection(const Ellipsoid& ellipsoid)
    : ellipsoid_(ellipsoid),
      semimajorAxis_(ellipsoid.maximumRadius()),
      oneOverSemimajorAxis_(1.0 / ellipsoid.maximumRadius()) {}

Vec3 GeographicProjection::project(const Cartographic& cartographic) const {
    return Vec3(cartographic.longitude() * semimajorAxis_,
                cartographic.latitude() * semimajorAxis_,
                cartographic.height());
}

Rectangle GeographicProjection::project(const Rectangle& rectangle) const {
    const Vec3 sw = project(Cartographic(rectangle.west(), rectangle.south(), 0.0));
    const Vec3 ne = project(Cartographic(rectangle.east(), rectangle.north(), 0.0));
    return Rectangle(sw.x(), sw.y(), ne.x(), ne.y());
}

Cartographic GeographicProjection::unproject(
    const glm::dvec2& projectedCoordinates) const {
    return Cartographic(projectedCoordinates.x * oneOverSemimajorAxis_,
                        projectedCoordinates.y * oneOverSemimajorAxis_,
                        0.0);
}

Cartographic GeographicProjection::unproject(
    const Vec3& projectedCoordinates) const {
    Cartographic result = unproject(
        glm::dvec2(projectedCoordinates.x(), projectedCoordinates.y()));
    result.setHeight(projectedCoordinates.z());
    return result;
}

Rectangle GeographicProjection::unproject(const Rectangle& rectangle) const {
    const Cartographic sw = unproject(Vec3(rectangle.west(), rectangle.south(), 0.0));
    const Cartographic ne = unproject(Vec3(rectangle.east(), rectangle.north(), 0.0));
    return Rectangle(sw.longitude(), sw.latitude(), ne.longitude(), ne.latitude());
}

Rectangle GeographicProjection::maximumGlobeRectangle() {
    return Rectangle(-MathUtils::OnePi,
                     -MathUtils::PiOverTwo,
                     MathUtils::OnePi,
                     MathUtils::PiOverTwo);
}

Rectangle GeographicProjection::computeMaximumProjectedRectangle(
    const Ellipsoid& ellipsoid) {
    const double longitudeValue = ellipsoid.maximumRadius() * MathUtils::OnePi;
    const double latitudeValue =
        ellipsoid.maximumRadius() * MathUtils::PiOverTwo;
    return Rectangle(-longitudeValue, -latitudeValue, longitudeValue, latitudeValue);
}

} // namespace earth_engine
