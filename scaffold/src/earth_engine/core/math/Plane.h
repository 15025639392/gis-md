#pragma once

#include "Vec3.h"

namespace earth_engine {

/// A plane in Hessian Normal Form.  Aligned with cesium-native
/// CesiumGeometry::Plane for BoundingSphere::intersectPlane support.
///
/// Plane equation: normal · point + distance = 0
class Plane {
public:
    /// XY plane through origin, normal +Z
    static const Plane ORIGIN_XY;
    /// YZ plane through origin, normal +X
    static const Plane ORIGIN_YZ;
    /// ZX plane through origin, normal +Y
    static const Plane ORIGIN_ZX;

    Plane() noexcept : normal_(0, 0, 1), distance_(0) {}

    /// Construct from a normal (must be unit length) and signed distance
    /// from the origin.
    Plane(const Vec3& normal, double distance) noexcept
        : normal_(normal), distance_(distance) {}

    /// Construct from a point on the plane and the plane's normal (unit length).
    Plane(const Vec3& point, const Vec3& normal) noexcept
        : normal_(normal), distance_(-normal.dot(point)) {}

    const Vec3& getNormal() const noexcept { return normal_; }
    double getDistance() const noexcept { return distance_; }

    /// Signed distance from a point to the plane.
    /// Positive = in direction of normal, negative = opposite side.
    double getPointDistance(const Vec3& point) const noexcept {
        return normal_.dot(point) + distance_;
    }

private:
    Vec3 normal_{0, 0, 1};
    double distance_{0};
};

} // namespace earth_engine
