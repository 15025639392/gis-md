#pragma once

#include "../core/math/Mat4.h"
#include "../core/math/Ray.h"
#include "../core/math/Vec3.h"
#include "Frustum.h"

namespace earth_engine {

/// Perspective camera in ECEF/world meters.
/// Screen coordinates are physical viewport pixels with origin at top-left.
class Camera {
public:
    Camera();

    const Vec3& position() const { return position_; }
    const Vec3& direction() const { return direction_; }
    const Vec3& up() const { return up_; }
    const Vec3& right() const { return right_; }

    double verticalFovRadians() const { return verticalFovRadians_; }
    double nearPlaneMeters() const { return nearPlaneMeters_; }
    double farPlaneMeters() const { return farPlaneMeters_; }

    /// 是否正交投影（当前仅支持透视）
    bool isOrthographic() const { return false; }

    /// 相机距 WGS84 椭球表面的高度（米）
    double getHeight() const;

    void setView(const Vec3& position, const Vec3& direction, const Vec3& up);
    void lookAt(const Vec3& position, const Vec3& target, const Vec3& up);
    const Vec3& target() const { return target_; }
    void setPerspective(double verticalFovRadians,
                        double nearPlaneMeters,
                        double farPlaneMeters);

    Mat4 viewMatrix() const;
    Mat4 projectionMatrix(double viewportWidthPixels,
                          double viewportHeightPixels) const;
    Mat4 viewProjectionMatrix(double viewportWidthPixels,
                              double viewportHeightPixels) const;
    Frustum frustum(double viewportWidthPixels,
                    double viewportHeightPixels) const;

    Ray getPickRay(double screenXPixels,
                   double screenYPixels,
                   double viewportWidthPixels,
                   double viewportHeightPixels) const;

private:
    void setOrientation(const Vec3& direction, const Vec3& up);

    Vec3 position_;
    Vec3 direction_;
    Vec3 up_;
    Vec3 right_;
    Vec3 target_;
    double verticalFovRadians_;
    double nearPlaneMeters_;
    double farPlaneMeters_;
};

} // namespace earth_engine
