#include "Plane.h"

#include <cmath>

namespace earth_engine {

namespace {
constexpr double kNormalUnitEpsilon = 1e-6;
}

const Plane Plane::ORIGIN_XY{Vec3::unitZ(), 0.0};
const Plane Plane::ORIGIN_YZ{Vec3::unitX(), 0.0};
const Plane Plane::ORIGIN_ZX{Vec3::unitY(), 0.0};

Plane::Plane(const Vec3& normal, double distance)
    : normal_(normal), distance_(distance) {
    if (std::abs(normal.length() - 1.0) > kNormalUnitEpsilon) {
        throw std::invalid_argument("Plane normal must be normalized.");
    }
}

Plane::Plane(const Vec3& point, const Vec3& normal)
    : Plane(normal, -normal.dot(point)) {}

Vec3 Plane::projectPointOntoPlane(const Vec3& point) const noexcept {
    return point - normal_ * getPointDistance(point);
}

} // namespace earth_engine
