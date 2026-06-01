#include "Vec3.h"
#include <cmath>

namespace earth_engine {

double Vec3::length() const {
    return glm::length(v_);
}

double Vec3::lengthSquared() const {
    return glm::dot(v_, v_);
}

Vec3 Vec3::normalized() const {
    return Vec3(glm::normalize(v_));
}

double Vec3::dot(const Vec3& rhs) const {
    return glm::dot(v_, rhs.v_);
}

Vec3 Vec3::cross(const Vec3& rhs) const {
    return Vec3(glm::cross(v_, rhs.v_));
}

double Vec3::distanceTo(const Vec3& rhs) const {
    return glm::distance(v_, rhs.v_);
}

Vec3 operator*(double s, const Vec3& v) {
    return Vec3(s * v.raw());
}

std::ostream& operator<<(std::ostream& os, const Vec3& v) {
    return os << "Vec3(" << v.x() << ", " << v.y() << ", " << v.z() << ")";
}

} // namespace earth_engine
