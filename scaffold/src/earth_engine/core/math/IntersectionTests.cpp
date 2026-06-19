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

std::optional<RayEllipsoidIntersectionInterval>
IntersectionTests::rayEllipsoid(const Ray& ray, const Vec3& radii) noexcept {
    if (radii.x() == 0.0 || radii.y() == 0.0 || radii.z() == 0.0) {
        return std::nullopt;
    }

    const Vec3 inverseRadii(1.0 / radii.x(), 1.0 / radii.y(), 1.0 / radii.z());
    const Vec3 q(ray.origin().x() * inverseRadii.x(),
                 ray.origin().y() * inverseRadii.y(),
                 ray.origin().z() * inverseRadii.z());
    const Vec3 w(ray.direction().x() * inverseRadii.x(),
                 ray.direction().y() * inverseRadii.y(),
                 ray.direction().z() * inverseRadii.z());

    const double q2 = q.dot(q);
    const double qw = q.dot(w);
    const double w2 = w.dot(w);
    if (w2 <= 0.0) {
        return std::nullopt;
    }

    if (q2 > 1.0) {
        if (qw >= 0.0) {
            return std::nullopt;
        }

        const double qw2 = qw * qw;
        const double difference = q2 - 1.0;
        const double product = w2 * difference;

        if (qw2 < product) {
            return std::nullopt;
        }
        if (qw2 > product) {
            const double discriminant = qw2 - product;
            const double temp = -qw + std::sqrt(discriminant);
            const double root0 = temp / w2;
            const double root1 = difference / temp;
            return root0 < root1
                ? RayEllipsoidIntersectionInterval{root0, root1}
                : RayEllipsoidIntersectionInterval{root1, root0};
        }

        const double root = std::sqrt(difference / w2);
        return RayEllipsoidIntersectionInterval{root, root};
    }

    if (q2 < 1.0) {
        const double difference = q2 - 1.0;
        const double product = w2 * difference;
        const double discriminant = qw * qw - product;
        const double temp = -qw + std::sqrt(discriminant);
        return RayEllipsoidIntersectionInterval{0.0, temp / w2};
    }

    if (qw < 0.0) {
        return RayEllipsoidIntersectionInterval{0.0, -qw / w2};
    }

    return std::nullopt;
}

} // namespace earth_engine
