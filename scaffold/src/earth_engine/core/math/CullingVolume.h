#pragma once

#include "Mat4.h"
#include "Plane.h"
#include "Vec3.h"

namespace earth_engine {

struct CullingVolume {
    Plane leftPlane{Vec3::unitZ(), 0.0};
    Plane rightPlane{Vec3::unitZ(), 0.0};
    Plane topPlane{Vec3::unitZ(), 0.0};
    Plane bottomPlane{Vec3::unitZ(), 0.0};

    static CullingVolume fromPerspectiveFieldOfView(
        const Vec3& position,
        const Vec3& direction,
        const Vec3& up,
        double horizontalFovRadians,
        double verticalFovRadians);

    static CullingVolume fromPerspectiveOffCenter(
        const Vec3& position,
        const Vec3& direction,
        const Vec3& up,
        double left,
        double right,
        double bottom,
        double top,
        double nearPlane);

    static CullingVolume fromClipMatrix(const Mat4& clipMatrix);
};

} // namespace earth_engine
