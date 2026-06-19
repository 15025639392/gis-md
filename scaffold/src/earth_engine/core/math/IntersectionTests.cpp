#include "IntersectionTests.h"

#include <cmath>

namespace earth_engine {

std::optional<Vec3> IntersectionTests::rayPlane(const Ray& ray, const Plane& plane) noexcept {
    constexpr double epsilon15 = 1e-15;
    const Vec3& normal = plane.getNormal();
    const double denominator = normal.dot(ray.direction());

    if (std::abs(denominator) < epsilon15) {
        return std::nullopt;
    }

    const double t = (-plane.getDistance() - normal.dot(ray.origin())) / denominator;
    if (t < 0.0) {
        return std::nullopt;
    }

    return ray.origin() + ray.direction() * t;
}

} // namespace earth_engine
