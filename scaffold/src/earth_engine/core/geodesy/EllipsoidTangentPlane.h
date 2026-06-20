#pragma once

#include "Ellipsoid.h"
#include "../math/Mat4.h"
#include "../math/Plane.h"
#include "../math/Vec3.h"

#include <glm/ext/vector_double2.hpp>

namespace earth_engine {

/// Plane tangent to an ellipsoid at a surface-projected origin.
/// Aligned with cesium-native CesiumGeospatial::EllipsoidTangentPlane.
class EllipsoidTangentPlane {
public:
    explicit EllipsoidTangentPlane(
        const Vec3& origin,
        const Ellipsoid& ellipsoid = Ellipsoid::WGS84());
    explicit EllipsoidTangentPlane(
        const Mat4& eastNorthUpToFixedFrame,
        const Ellipsoid& ellipsoid = Ellipsoid::WGS84());

    const Ellipsoid& getEllipsoid() const noexcept { return ellipsoid_; }
    const Vec3& getOrigin() const noexcept { return origin_; }
    const Vec3& getXAxis() const noexcept { return xAxis_; }
    const Vec3& getYAxis() const noexcept { return yAxis_; }
    const Vec3& getZAxis() const noexcept { return plane_.getNormal(); }
    const Plane& getPlane() const noexcept { return plane_; }

    glm::dvec2 projectPointToNearestOnPlane(
        const Vec3& cartesian) const noexcept;

private:
    static Mat4 computeEastNorthUpToFixedFrame(
        const Vec3& origin,
        const Ellipsoid& ellipsoid);

    Ellipsoid ellipsoid_;
    Vec3 origin_;
    Vec3 xAxis_;
    Vec3 yAxis_;
    Plane plane_;
};

} // namespace earth_engine
