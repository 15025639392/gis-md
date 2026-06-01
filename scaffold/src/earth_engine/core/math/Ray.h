#pragma once

#include "Vec3.h"
#include <ostream>

namespace earth_engine {

/// 射线：origin + t * direction（direction 已归一化）
class Ray {
public:
    Ray() : origin_(), direction_(Vec3::unitZ()) {}
    Ray(const Vec3& origin, const Vec3& direction)
        : origin_(origin), direction_(direction.normalized()) {}

    const Vec3& origin() const { return origin_; }
    const Vec3& direction() const { return direction_; }

    /// 射线上的点：origin + t * direction
    Vec3 pointAt(double t) const;

private:
    Vec3 origin_;
    Vec3 direction_;
};

std::ostream& operator<<(std::ostream& os, const Ray& r);

} // namespace earth_engine
