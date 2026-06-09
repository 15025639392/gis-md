#pragma once

#include "Vec3.h"
#include "Mat4.h"
#include "Plane.h"

namespace earth_engine {

/// An oriented bounding box defined by a center and three half-axis vectors.
/// Aligned with cesium-native CesiumGeometry::OrientedBoundingBox for
/// tight-fitting frustum culling.
///
/// The half-axes are column vectors of a 3x3 matrix, each giving direction
/// and half-extent along that axis. The 8 corners are:
///   center ± axis0 ± axis1 ± axis2
class OrientedBoundingBox {
public:
    /// Construct from center and three half-axis vectors.
    OrientedBoundingBox(const Vec3& center,
                        const Vec3& axis0,
                        const Vec3& axis1,
                        const Vec3& axis2) noexcept
        : center_(center) {
        halfAxes_[0] = axis0;
        halfAxes_[1] = axis1;
        halfAxes_[2] = axis2;
    }

    const Vec3& getCenter() const noexcept { return center_; }
    const Vec3& getHalfAxis(int i) const noexcept { return halfAxes_[i]; }

    /// Returns: -1 = Outside, 1 = Inside, 0 = Intersecting.
    /// Same semantics as cesium-native CullingResult.
    int intersectPlane(const Plane& plane) const noexcept {
        const Vec3& n = plane.getNormal();

        // Project each half-axis onto the plane normal. The effective radius
        // is the sum of absolute projections (separating axis theorem).
        double radEffective = 0.0;
        for (int i = 0; i < 3; ++i) {
            radEffective += std::abs(n.dot(halfAxes_[i]));
        }

        const double distanceToPlane = n.dot(center_) + plane.getDistance();

        if (distanceToPlane < -radEffective) return -1;  // Outside
        if (distanceToPlane > radEffective)  return 1;   // Inside
        return 0;  // Intersecting
    }

    /// Test if the OBB is entirely outside any of the 6 frustum planes.
    /// Returns true if the box IS visible (intersects or inside all planes).
    bool isVisible(const Plane planes[6]) const noexcept {
        for (int i = 0; i < 6; ++i) {
            if (intersectPlane(planes[i]) < 0) return false;
        }
        return true;
    }

    /// Compute squared distance from a point to the closest point on the OBB.
    double computeDistanceSquaredToPosition(const Vec3& position) const noexcept {
        const Vec3 offset = position - center_;

        // Project offset onto each axis to get local coordinates
        double p0 = offset.dot(halfAxes_[0].normalized());
        double p1 = offset.dot(halfAxes_[1].normalized());
        double p2 = offset.dot(halfAxes_[2].normalized());

        double h0 = halfAxes_[0].length();
        double h1 = halfAxes_[1].length();
        double h2 = halfAxes_[2].length();

        double dSq = 0.0;
        auto clampDist = [&](double p, double h) {
            if (p < -h) { double d = p + h; dSq += d * d; }
            else if (p > h) { double d = p - h; dSq += d * d; }
        };
        clampDist(p0, h0);
        clampDist(p1, h1);
        clampDist(p2, h2);

        return dSq;
    }

    /// Convert to a bounding sphere (for existing sphere-based culling paths).
    BoundingSphere toSphere() const noexcept {
        double radius = (halfAxes_[0] + halfAxes_[1] + halfAxes_[2]).length();
        return BoundingSphere(center_, radius);
    }

private:
    Vec3 center_;
    Vec3 halfAxes_[3];
};

} // namespace earth_engine
