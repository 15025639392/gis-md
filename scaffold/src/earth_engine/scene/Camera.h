#pragma once

#include "../core/math/Mat4.h"
#include "../core/math/Ray.h"
#include "../core/math/Vec3.h"
#include "Frustum.h"

namespace earth_engine {

/// Camera in ECEF/world meters (perspective or orthographic).
/// Screen coordinates are physical viewport pixels with origin at top-left.
class Camera {
public:
    /// 投影模式。正交与透视**共用同一套 reverse-Z 深度约定**(near→z_ndc=1,
    /// far→0,depth clear=0 + GreaterEqual),故切换模式不需要动任何深度状态。
    enum class ProjectionMode { Perspective, Orthographic };

    Camera();

    const Vec3& position() const { return position_; }
    const Vec3& direction() const { return direction_; }
    const Vec3& up() const { return up_; }
    const Vec3& right() const { return right_; }

    /// ⚠️ 正交下**没有 fov 这回事**,本值只是切模式前的残留。任何"像素→角度"
    /// 的换算(转台增益、SSE)在正交下都不该读它 —— 见 `orthographicWidthMeters`。
    double verticalFovRadians() const { return verticalFovRadians_; }
    double nearPlaneMeters() const { return nearPlaneMeters_; }
    double farPlaneMeters() const { return farPlaneMeters_; }

    ProjectionMode projectionMode() const { return projectionMode_; }
    bool isOrthographic() const {
        return projectionMode_ == ProjectionMode::Orthographic;
    }
    /// 正交视口的世界宽度(米)。高度按视口宽高比推出。
    double orthographicWidthMeters() const { return orthographicWidthMeters_; }

    /// 相机距 WGS84 椭球表面的高度（米）
    double getHeight() const;

    /// 法线矩阵（3×3，viewMatrix 的旋转部分，column-major，9 floats）
    void getNormalMatrix(float out[9]) const;

    void setView(const Vec3& position, const Vec3& direction, const Vec3& up);
    void lookAt(const Vec3& position, const Vec3& target, const Vec3& up);
    const Vec3& target() const { return target_; }
    void setPerspective(double verticalFovRadians,
                        double nearPlaneMeters,
                        double farPlaneMeters);

    /// 切到正交投影。
    /// @param orthographicWidthMeters 视口覆盖的世界宽度(米),须 > 0
    /// ⚠️ 正交下 near 可以 ≤ 0(相机平面之后的东西照样在盒子里),但为了与透视
    /// 共用同一套接口与深度约定,这里仍要求 0 < near < far。
    void setOrthographic(double orthographicWidthMeters,
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
    ProjectionMode projectionMode_;
    double verticalFovRadians_;
    double orthographicWidthMeters_;
    double nearPlaneMeters_;
    double farPlaneMeters_;
};

} // namespace earth_engine
