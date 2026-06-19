#pragma once

#include "Plane.h"
#include "Ray.h"
#include "RayEllipsoidIntersectionInterval.h"
#include "Vec3.h"

#include <optional>

namespace earth_engine {

class AxisAlignedBox;
class BoundingSphere;
class OrientedBoundingBox;

/// Geometry intersection helpers aligned with cesium-native IntersectionTests.
class IntersectionTests {
public:
    IntersectionTests() = delete;

    static std::optional<Vec3> rayPlane(const Ray& ray, const Plane& plane) noexcept;
    static std::optional<RayEllipsoidIntersectionInterval>
    rayEllipsoid(const Ray& ray, const Vec3& radii) noexcept;
    static std::optional<Vec3> rayTriangle(const Ray& ray,
                                           const Vec3& v0,
                                           const Vec3& v1,
                                           const Vec3& v2,
                                           bool cullBackFaces);
    static std::optional<double> rayTriangleParametric(const Ray& ray,
                                                       const Vec3& p0,
                                                       const Vec3& p1,
                                                       const Vec3& p2,
                                                       bool cullBackFaces);
    static std::optional<Vec3> rayAABB(const Ray& ray, const AxisAlignedBox& aabb);
    static std::optional<double> rayAABBParametric(const Ray& ray,
                                                   const AxisAlignedBox& aabb);
    static std::optional<Vec3> rayOBB(const Ray& ray, const OrientedBoundingBox& obb);
    static std::optional<double> rayOBBParametric(const Ray& ray,
                                                  const OrientedBoundingBox& obb);
    static std::optional<Vec3> raySphere(const Ray& ray, const BoundingSphere& sphere);
    static std::optional<double> raySphereParametric(const Ray& ray,
                                                     const BoundingSphere& sphere);
};

} // namespace earth_engine
