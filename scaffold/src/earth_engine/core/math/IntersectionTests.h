#pragma once

#include "Plane.h"
#include "Ray.h"
#include "RayEllipsoidIntersectionInterval.h"
#include "Vec3.h"

#include <optional>

namespace earth_engine {

/// Geometry intersection helpers aligned with cesium-native IntersectionTests.
class IntersectionTests {
public:
    IntersectionTests() = delete;

    static std::optional<Vec3> rayPlane(const Ray& ray, const Plane& plane) noexcept;
    static std::optional<RayEllipsoidIntersectionInterval>
    rayEllipsoid(const Ray& ray, const Vec3& radii) noexcept;
};

} // namespace earth_engine
