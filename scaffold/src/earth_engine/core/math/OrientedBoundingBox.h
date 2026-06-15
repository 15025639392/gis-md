#pragma once

#include "BoundingSphere.h"
#include "Vec3.h"
#include "Mat4.h"
#include "Plane.h"

#include <cmath>

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

        if (distanceToPlane <= -radEffective) return -1;  // Outside
        if (distanceToPlane >= radEffective)  return 1;   // Inside
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

        Vec3 u = halfAxes_[0];
        Vec3 v = halfAxes_[1];
        Vec3 w = halfAxes_[2];

        const double h0 = u.length();
        const double h1 = v.length();
        const double h2 = w.length();
        const bool uValid = h0 > 0.0;
        const bool vValid = h1 > 0.0;
        const bool wValid = h2 > 0.0;

        if (uValid) u = u / h0;
        if (vValid) v = v / h1;
        if (wValid) w = w / h2;

        const int degenerateAxes =
            (uValid ? 0 : 1) + (vValid ? 0 : 1) + (wValid ? 0 : 1);
        if (degenerateAxes == 1) {
            Vec3 validAxis1 = v;
            Vec3 validAxis2 = w;
            if (!vValid) {
                validAxis1 = u;
            } else if (!wValid) {
                validAxis2 = u;
            }

            const Vec3 validAxis3 = validAxis1.cross(validAxis2);
            if (!uValid) {
                u = validAxis3;
            } else if (!vValid) {
                v = validAxis3;
            } else {
                w = validAxis3;
            }
        } else if (degenerateAxes == 2) {
            Vec3 validAxis1 = uValid ? u : (vValid ? v : w);
            Vec3 crossVector = Vec3::unitY();
            if (std::abs(validAxis1.dot(crossVector)) > 1.0 - 1e-3) {
                crossVector = Vec3::unitX();
            }

            const Vec3 validAxis2 = validAxis1.cross(crossVector).normalized();
            const Vec3 validAxis3 = validAxis1.cross(validAxis2).normalized();
            if (uValid) {
                v = validAxis2;
                w = validAxis3;
            } else if (vValid) {
                w = validAxis2;
                u = validAxis3;
            } else {
                u = validAxis2;
                v = validAxis3;
            }
        } else if (degenerateAxes == 3) {
            u = Vec3::unitX();
            v = Vec3::unitY();
            w = Vec3::unitZ();
        }

        const double p0 = offset.dot(u);
        const double p1 = offset.dot(v);
        const double p2 = offset.dot(w);

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
