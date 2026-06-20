#pragma once

#include "Mat4.h"
#include "Vec3.h"
#include <ostream>
#include <stdexcept>

namespace earth_engine {

/// 射线：origin + t * direction（direction 已归一化）
class Ray {
public:
    Ray(const Vec3& origin, const Vec3& direction);

    const Vec3& origin() const { return origin_; }
    const Vec3& direction() const { return direction_; }

    /// 射线上的点：origin + t * direction
    Vec3 pointAt(double t) const;

    /// Transform origin as a point and direction as a vector, matching
    /// cesium-native CesiumGeometry::Ray::transform semantics.
    Ray transform(const Mat4& transformation) const;

    Ray operator-() const;

private:
    Vec3 origin_;
    Vec3 direction_;
};

std::ostream& operator<<(std::ostream& os, const Ray& r);

} // namespace earth_engine
