#include "SimplePlanarEllipsoidCurve.h"

#include <algorithm>
#include <cmath>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>

namespace earth_engine {

namespace {

Vec3 orthogonalAxis(const Vec3& direction) {
    const Vec3 candidate =
        std::abs(direction.x()) < 0.9 ? Vec3::unitX() : Vec3::unitY();
    return direction.cross(candidate).normalized();
}

} // namespace

std::optional<SimplePlanarEllipsoidCurve>
SimplePlanarEllipsoidCurve::fromEarthCenteredEarthFixedCoordinates(
    const Ellipsoid& ellipsoid,
    const Vec3& sourceEcef,
    const Vec3& destinationEcef) {
    const std::optional<Vec3> scaledSourceEcef =
        ellipsoid.tryScaleToGeocentricSurface(sourceEcef);
    const std::optional<Vec3> scaledDestinationEcef =
        ellipsoid.tryScaleToGeocentricSurface(destinationEcef);

    if (!scaledSourceEcef || !scaledDestinationEcef) {
        return std::nullopt;
    }

    return SimplePlanarEllipsoidCurve(ellipsoid,
                                      *scaledSourceEcef,
                                      *scaledDestinationEcef,
                                      sourceEcef,
                                      destinationEcef);
}

std::optional<SimplePlanarEllipsoidCurve>
SimplePlanarEllipsoidCurve::fromLongitudeLatitudeHeight(
    const Ellipsoid& ellipsoid,
    const Cartographic& source,
    const Cartographic& destination) {
    return fromEarthCenteredEarthFixedCoordinates(
        ellipsoid,
        ellipsoid.cartographicToCartesian(source),
        ellipsoid.cartographicToCartesian(destination));
}

Vec3 SimplePlanarEllipsoidCurve::getPosition(double percentage,
                                             double additionalHeight) const {
    if (percentage <= 0.0) {
        return sourceEcef_;
    }
    if (percentage >= 1.0) {
        return destinationEcef_;
    }

    percentage = std::clamp(percentage, 0.0, 1.0);

    const glm::dvec3 rotatedDirection =
        glm::angleAxis(percentage * totalAngle_, rotationAxis_.raw()) *
        sourceDirection_.raw();

    const Vec3 geocentricPosition =
        ellipsoid_.tryScaleToGeocentricSurface(Vec3(rotatedDirection))
            .value_or(Vec3::zero());
    const Vec3 geocentricUp = geocentricPosition.normalized();
    const double altitudeOffset =
        sourceHeight_ +
        (destinationHeight_ - sourceHeight_) * percentage +
        additionalHeight;

    return geocentricPosition + geocentricUp * altitudeOffset;
}

SimplePlanarEllipsoidCurve::SimplePlanarEllipsoidCurve(
    const Ellipsoid& ellipsoid,
    const Vec3& scaledSourceEcef,
    const Vec3& scaledDestinationEcef,
    const Vec3& originalSourceEcef,
    const Vec3& originalDestinationEcef)
    : totalAngle_(0.0),
      sourceHeight_(originalSourceEcef.length() - scaledSourceEcef.length()),
      destinationHeight_(
          originalDestinationEcef.length() - scaledDestinationEcef.length()),
      ellipsoid_(ellipsoid),
      sourceDirection_(originalSourceEcef.normalized()),
      sourceEcef_(originalSourceEcef),
      destinationEcef_(originalDestinationEcef) {
    const Vec3 source = scaledSourceEcef.normalized();
    const Vec3 destination = scaledDestinationEcef.normalized();
    const double dot = std::clamp(source.dot(destination), -1.0, 1.0);

    totalAngle_ = std::acos(dot);
    if (totalAngle_ == 0.0) {
        rotationAxis_ = Vec3::unitZ();
    } else if (std::abs(dot + 1.0) <= 1e-15) {
        rotationAxis_ = orthogonalAxis(source);
        totalAngle_ = glm::pi<double>();
    } else {
        rotationAxis_ = source.cross(destination).normalized();
    }
}

} // namespace earth_engine
