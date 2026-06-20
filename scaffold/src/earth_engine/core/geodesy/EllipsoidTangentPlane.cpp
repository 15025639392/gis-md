#include "EllipsoidTangentPlane.h"

#include "Transforms.h"
#include "../math/IntersectionTests.h"
#include "../math/Ray.h"

#include <stdexcept>

namespace earth_engine {

EllipsoidTangentPlane::EllipsoidTangentPlane(
    const Vec3& origin,
    const Ellipsoid& ellipsoid)
    : EllipsoidTangentPlane(
          computeEastNorthUpToFixedFrame(origin, ellipsoid),
          ellipsoid) {}

EllipsoidTangentPlane::EllipsoidTangentPlane(
    const Mat4& eastNorthUpToFixedFrame,
    const Ellipsoid& ellipsoid)
    : ellipsoid_(ellipsoid),
      origin_(eastNorthUpToFixedFrame(0, 3),
              eastNorthUpToFixedFrame(1, 3),
              eastNorthUpToFixedFrame(2, 3)),
      xAxis_(eastNorthUpToFixedFrame(0, 0),
             eastNorthUpToFixedFrame(1, 0),
             eastNorthUpToFixedFrame(2, 0)),
      yAxis_(eastNorthUpToFixedFrame(0, 1),
             eastNorthUpToFixedFrame(1, 1),
             eastNorthUpToFixedFrame(2, 1)),
      plane_(origin_,
             Vec3(eastNorthUpToFixedFrame(0, 2),
                  eastNorthUpToFixedFrame(1, 2),
                  eastNorthUpToFixedFrame(2, 2))) {}

glm::dvec2 EllipsoidTangentPlane::projectPointToNearestOnPlane(
    const Vec3& cartesian) const noexcept {
    const Ray ray(cartesian, plane_.getNormal());

    std::optional<Vec3> intersectionPoint =
        IntersectionTests::rayPlane(ray, plane_);
    if (!intersectionPoint) {
        intersectionPoint = IntersectionTests::rayPlane(-ray, plane_);
        if (!intersectionPoint) {
            intersectionPoint = cartesian;
        }
    }

    const Vec3 v = *intersectionPoint - origin_;
    return glm::dvec2(xAxis_.dot(v), yAxis_.dot(v));
}

Mat4 EllipsoidTangentPlane::computeEastNorthUpToFixedFrame(
    const Vec3& origin,
    const Ellipsoid& ellipsoid) {
    const std::optional<Vec3> scaledOrigin =
        ellipsoid.tryScaleToGeodeticSurface(origin);
    if (!scaledOrigin) {
        throw std::invalid_argument(
            "The origin must not be near the center of the ellipsoid.");
    }
    return Transforms::eastNorthUpToFixedFrame(*scaledOrigin, ellipsoid);
}

} // namespace earth_engine
