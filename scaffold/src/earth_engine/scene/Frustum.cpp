#include "Frustum.h"
#include "../core/math/Plane.h"

#include <cmath>
#include <stdexcept>

namespace earth_engine {

namespace {

FrustumPlane makePlane(double a, double b, double c, double d) {
    const double length = std::sqrt(a * a + b * b + c * c);
    if (length <= 0.0) {
        throw std::invalid_argument("Frustum plane normal must be non-zero.");
    }
    return FrustumPlane{Vec3(a / length, b / length, c / length), d / length};
}

} // namespace

double FrustumPlane::signedDistanceTo(const Vec3& point) const {
    return normal.dot(point) + distance;
}

Frustum Frustum::fromViewProjection(const Mat4& viewProjection) {
    Frustum frustum;

    // GLM matrices are column-major, while Mat4(row, col) gives row-major access.
    const double m00 = viewProjection(0, 0);
    const double m01 = viewProjection(0, 1);
    const double m02 = viewProjection(0, 2);
    const double m03 = viewProjection(0, 3);
    const double m10 = viewProjection(1, 0);
    const double m11 = viewProjection(1, 1);
    const double m12 = viewProjection(1, 2);
    const double m13 = viewProjection(1, 3);
    const double m20 = viewProjection(2, 0);
    const double m21 = viewProjection(2, 1);
    const double m22 = viewProjection(2, 2);
    const double m23 = viewProjection(2, 3);
    const double m30 = viewProjection(3, 0);
    const double m31 = viewProjection(3, 1);
    const double m32 = viewProjection(3, 2);
    const double m33 = viewProjection(3, 3);

    frustum.planes_[static_cast<size_t>(PlaneIndex::Left)] =
        makePlane(m30 + m00, m31 + m01, m32 + m02, m33 + m03);
    frustum.planes_[static_cast<size_t>(PlaneIndex::Right)] =
        makePlane(m30 - m00, m31 - m01, m32 - m02, m33 - m03);
    frustum.planes_[static_cast<size_t>(PlaneIndex::Bottom)] =
        makePlane(m30 + m10, m31 + m11, m32 + m12, m33 + m13);
    frustum.planes_[static_cast<size_t>(PlaneIndex::Top)] =
        makePlane(m30 - m10, m31 - m11, m32 - m12, m33 - m13);
    // Reverse-Z depth [0,1]: near clips at z_ndc > 0 (row3), far at z_ndc < 1 (row4-row3).
    // Standard [-1,1] formula would be: near = row4+row3, far = row4-row3.
    frustum.planes_[static_cast<size_t>(PlaneIndex::Near)] =
        makePlane(m20, m21, m22, m23);               // z_clip > 0  → row3
    frustum.planes_[static_cast<size_t>(PlaneIndex::Far)] =
        makePlane(m30 - m20, m31 - m21, m32 - m22, m33 - m23); // z_clip < w_clip → row4-row3

    return frustum;
}

const FrustumPlane& Frustum::plane(PlaneIndex index) const {
    return planes_[static_cast<size_t>(index)];
}

bool Frustum::containsPoint(const Vec3& point, double epsilonMeters) const {
    for (const auto& p : planes_) {
        if (p.signedDistanceTo(point) < -epsilonMeters) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersectsSphere(const Vec3& center,
                               double radiusMeters,
                               double epsilonMeters) const {
    if (radiusMeters < 0.0) {
        throw std::invalid_argument("Sphere radius must be non-negative.");
    }

    for (const auto& p : planes_) {
        if (p.signedDistanceTo(center) < -(radiusMeters + epsilonMeters)) {
            return false;
        }
    }
    return true;
}

bool Frustum::intersectsSphere(const BoundingSphere& sphere,
                               double epsilonMeters) const {
    return intersectsSphere(
        sphere.getCenter(), sphere.getRadius(), epsilonMeters);
}

bool Frustum::intersectsOBB(const OrientedBoundingBox& obb) const {
    for (const auto& fp : planes_) {
        Plane plane(fp.normal, fp.distance);
        if (obb.intersectPlane(plane) < 0) return false;
    }
    return true;
}

} // namespace earth_engine
