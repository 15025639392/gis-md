#pragma once

#include "Vec3.h"
#include "Plane.h"

namespace earth_engine {

/// A bounding sphere with a center and a radius.
/// Aligned with cesium-native CesiumGeometry::BoundingSphere for culling.
class BoundingSphere {
public:
    /// Construct a new bounding sphere.
    constexpr BoundingSphere(const Vec3& center, double radius) noexcept
        : center_(center), radius_(radius) {}

    constexpr const Vec3& getCenter() const noexcept { return center_; }
    constexpr double getRadius() const noexcept { return radius_; }

    /// Determines on which side of a plane this sphere is located.
    /// Returns: < 0 = Inside (entire sphere on normal side),
    ///          > 0 = Outside (entire sphere on opposite side),
    ///          == 0 = Intersecting.
    int intersectPlane(const Plane& plane) const noexcept {
        const double d = plane.getPointDistance(center_);
        if (d < -radius_) return -1;  // Inside
        if (d > radius_) return 1;    // Outside
        return 0;                      // Intersecting
    }

    /// Computes the squared distance from a position to the closest point
    /// on this sphere. Returns 0 if the point is inside the sphere.
    double computeDistanceSquaredToPosition(const Vec3& position) const noexcept {
        double diffLength = (position - center_).length();
        if (diffLength <= radius_) return 0.0;
        double d = diffLength - radius_;
        return d * d;
    }

    /// Computes whether the given position is contained within the sphere.
    bool contains(const Vec3& position) const noexcept {
        return (position - center_).lengthSquared() <= radius_ * radius_;
    }

private:
    Vec3 center_;
    double radius_;
};

} // namespace earth_engine
