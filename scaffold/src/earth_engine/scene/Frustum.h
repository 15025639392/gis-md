#pragma once

#include "../core/math/BoundingSphere.h"
#include "../core/math/Mat4.h"
#include "../core/math/OrientedBoundingBox.h"
#include "../core/math/Vec3.h"
#include <array>

namespace earth_engine {

/// Normalized plane in world/ECEF meters: normal.dot(point) + distance >= 0 is inside.
struct FrustumPlane {
    Vec3 normal = Vec3::unitX();
    double distance = 0.0;

    double signedDistanceTo(const Vec3& point) const;
};

/// Six world-space frustum planes extracted from a view-projection matrix.
class Frustum {
public:
    enum class PlaneIndex {
        Left = 0,
        Right = 1,
        Bottom = 2,
        Top = 3,
        Near = 4,
        Far = 5
    };

    static Frustum fromViewProjection(const Mat4& viewProjection);

    const FrustumPlane& plane(PlaneIndex index) const;
    bool containsPoint(const Vec3& point, double epsilonMeters = 0.0) const;
    bool intersectsSphere(const Vec3& center,
                          double radiusMeters,
                          double epsilonMeters = 0.0) const;

    /// cesium-native aligned: test a BoundingSphere against all 6 frustum planes.
    bool intersectsSphere(const BoundingSphere& sphere,
                          double epsilonMeters = 0.0) const;

    /// cesium-native aligned: OBB test (tighter than sphere) against all 6 frustum planes.
    bool intersectsOBB(const OrientedBoundingBox& obb) const;

private:
    std::array<FrustumPlane, 6> planes_;
};

} // namespace earth_engine
