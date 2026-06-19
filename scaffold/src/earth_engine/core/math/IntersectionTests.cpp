#include "IntersectionTests.h"

#include "AxisAlignedBox.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace earth_engine {

namespace {
double component(const Vec3& v, int index) noexcept {
    switch (index) {
    case 0:
        return v.x();
    case 1:
        return v.y();
    default:
        return v.z();
    }
}
} // namespace

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

std::optional<Vec3> IntersectionTests::rayTriangle(const Ray& ray,
                                                   const Vec3& v0,
                                                   const Vec3& v1,
                                                   const Vec3& v2,
                                                   bool cullBackFaces) {
    const std::optional<double> t =
        rayTriangleParametric(ray, v0, v1, v2, cullBackFaces);
    if (t && *t >= 0.0) {
        return ray.pointAt(*t);
    }
    return std::nullopt;
}

std::optional<double> IntersectionTests::rayTriangleParametric(const Ray& ray,
                                                               const Vec3& p0,
                                                               const Vec3& p1,
                                                               const Vec3& p2,
                                                               bool cullBackFaces) {
    constexpr double epsilon8 = 1e-8;
    const Vec3& origin = ray.origin();
    const Vec3& direction = ray.direction();

    const Vec3 edge0 = p1 - p0;
    const Vec3 edge1 = p2 - p0;

    const Vec3 p = direction.cross(edge1);
    const double det = edge0.dot(p);
    if (cullBackFaces) {
        if (det < epsilon8) {
            return std::nullopt;
        }

        const Vec3 tvec = origin - p0;
        const double u = tvec.dot(p);
        if (u < 0.0 || u > det) {
            return std::nullopt;
        }

        const Vec3 q = tvec.cross(edge0);
        const double v = direction.dot(q);
        if (v < 0.0 || u + v > det) {
            return std::nullopt;
        }

        return edge1.dot(q) / det;
    }

    if (std::abs(det) < epsilon8) {
        return std::nullopt;
    }

    const double invDet = 1.0 / det;
    const Vec3 tvec = origin - p0;
    const double u = tvec.dot(p) * invDet;
    if (u < 0.0 || u > 1.0) {
        return std::nullopt;
    }

    const Vec3 q = tvec.cross(edge0);
    const double v = direction.dot(q) * invDet;
    if (v < 0.0 || u + v > 1.0) {
        return std::nullopt;
    }

    return edge1.dot(q) * invDet;
}

std::optional<Vec3> IntersectionTests::rayAABB(const Ray& ray,
                                               const AxisAlignedBox& aabb) {
    const std::optional<double> t = rayAABBParametric(ray, aabb);
    if (t && *t >= 0.0) {
        return ray.pointAt(*t);
    }
    return std::nullopt;
}

std::optional<double> IntersectionTests::rayAABBParametric(
    const Ray& ray,
    const AxisAlignedBox& aabb) {
    constexpr double epsilon6 = 1e-6;
    const Vec3& dir = ray.direction();
    const Vec3& origin = ray.origin();
    const Vec3 minimum(aabb.minimumX(), aabb.minimumY(), aabb.minimumZ());
    const Vec3 maximum(aabb.maximumX(), aabb.maximumY(), aabb.maximumZ());

    double greatestMin = -std::numeric_limits<double>::max();
    double smallestMax = std::numeric_limits<double>::max();
    double tmin = greatestMin;
    double tmax = smallestMax;

    for (int i = 0; i < 3; ++i) {
        if (std::abs(component(dir, i)) < epsilon6) {
            continue;
        }

        tmin = (component(minimum, i) - component(origin, i)) / component(dir, i);
        tmax = (component(maximum, i) - component(origin, i)) / component(dir, i);

        if (tmin > tmax) {
            std::swap(tmin, tmax);
        }
        greatestMin = std::max(tmin, greatestMin);
        smallestMax = std::min(tmax, smallestMax);
    }

    if (smallestMax < 0.0 || greatestMin > smallestMax) {
        return std::nullopt;
    }
    return greatestMin < 0.0 ? smallestMax : greatestMin;
}

} // namespace earth_engine
