#include "Ray.h"

#include <cmath>
#include <glm/geometric.hpp>

namespace earth_engine {

namespace {
constexpr double kDirectionUnitEpsilon = 1e-6;
}

Ray::Ray(const Vec3& origin, const Vec3& direction)
    : origin_(origin), direction_(direction) {
    if (std::abs(direction.length() - 1.0) > kDirectionUnitEpsilon) {
        throw std::invalid_argument("Ray direction must be normalized.");
    }
}

Vec3 Ray::pointAt(double t) const {
    return origin_ + t * direction_;
}

Ray Ray::transform(const Mat4& transformation) const {
    const glm::dmat4& m = transformation.raw();
    const glm::dvec3 transformedOrigin =
        glm::dvec3(m * glm::dvec4(origin_.raw(), 1.0));
    const glm::dvec3 transformedDirection =
        glm::normalize(glm::dvec3(m * glm::dvec4(direction_.raw(), 0.0)));
    return Ray(Vec3(transformedOrigin), Vec3(transformedDirection));
}

Ray Ray::operator-() const {
    return Ray(origin_, -direction_);
}

std::ostream& operator<<(std::ostream& os, const Ray& r) {
    return os << "Ray(origin=" << r.origin() << ", dir=" << r.direction() << ")";
}

} // namespace earth_engine
