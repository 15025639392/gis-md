#include "Ray.h"

namespace earth_engine {

Vec3 Ray::pointAt(double t) const {
    return origin_ + t * direction_;
}

std::ostream& operator<<(std::ostream& os, const Ray& r) {
    return os << "Ray(origin=" << r.origin() << ", dir=" << r.direction() << ")";
}

} // namespace earth_engine
