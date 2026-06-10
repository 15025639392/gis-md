#include "Camera.h"
#include "../core/geodesy/Ellipsoid.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <stdexcept>
#include <limits>
#include <cmath>

namespace earth_engine {

namespace {
constexpr double kMinViewportPixels = 1.0;
}

Camera::Camera()
    : position_(0.0, 0.0, 7000000.0),
      direction_(0.0, 0.0, -1.0),
      up_(0.0, 1.0, 0.0),
      right_(1.0, 0.0, 0.0),
      target_(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0),
      verticalFovRadians_(glm::radians(60.0)),
      // OpenGlobus PlanetCamera defaults: near=1.0, far=1e12 with reverse-Z.
      nearPlaneMeters_(1.0),
      farPlaneMeters_(1e12) {}

void Camera::setView(const Vec3& position, const Vec3& direction, const Vec3& up) {
    position_ = position;
    target_ = Vec3(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0);  // invalidate stale target
    setOrientation(direction, up);
}

void Camera::lookAt(const Vec3& position, const Vec3& target, const Vec3& up) {
    position_ = position;
    target_ = target;
    setOrientation(target - position, up);
}

void Camera::setPerspective(double verticalFovRadians,
                            double nearPlaneMeters,
                            double farPlaneMeters) {
    if (verticalFovRadians <= 0.0 || verticalFovRadians >= glm::pi<double>()) {
        throw std::invalid_argument("Camera vertical FOV must be in (0, pi) radians.");
    }
    if (nearPlaneMeters <= 0.0 || farPlaneMeters <= nearPlaneMeters) {
        throw std::invalid_argument("Camera near/far planes must satisfy 0 < near < far.");
    }

    verticalFovRadians_ = verticalFovRadians;
    nearPlaneMeters_ = nearPlaneMeters;
    farPlaneMeters_ = farPlaneMeters;
}

Mat4 Camera::viewMatrix() const {
    // 如果已通过 lookAt 设置目标，使用目标点；否则回退到 position+direction
    glm::dvec3 center;
    if (std::isnan(target_.x())) {
        center = (position_ + direction_).raw();
    } else {
        center = target_.raw();
    }
    return Mat4(glm::lookAt(position_.raw(), center, up_.raw()));
}

Mat4 Camera::projectionMatrix(double viewportWidthPixels,
                              double viewportHeightPixels) const {
    if (viewportWidthPixels < kMinViewportPixels || viewportHeightPixels < kMinViewportPixels) {
        throw std::invalid_argument("Camera viewport dimensions must be positive pixels.");
    }

    // Reverse-Z projection, matched to OpenGlobus PlanetCamera
    // (reverseDepth: true, frustums: [[1, 1e12]]).
    //
    // Depth mapping: z_eye = near(1) → z_ndc = 1, z_eye = far(1e12) → z_ndc = 0.
    // Requires: depth clear = 0.0, depth func = GreaterEqual.
    //
    // Matrix (column-major, glm convention):
    //   P[2][2] =  near/(far-near)
    //   P[2][3] = -far*near/(far-near)
    //   P[3][2] = -1  (standard w_clip = -z_eye)

    const double aspect = viewportWidthPixels / viewportHeightPixels;
    const double f = 1.0 / std::tan(verticalFovRadians_ * 0.5);
    const double n = nearPlaneMeters_;
    const double r = farPlaneMeters_;  // "far" becomes the near limit in reverse-Z
    const double invRange = 1.0 / (r - n);

    glm::dmat4 proj(0.0);
    proj[0][0] = f / aspect;
    proj[1][1] = f;
    proj[2][2] = n * invRange;
    proj[2][3] = -1.0;                  // P[3][2] = -1 → w_clip = -z_eye
    proj[3][2] = r * n * invRange;      // P[2][3] = +far*near/(far-near)

    return Mat4(proj);
}

Mat4 Camera::viewProjectionMatrix(double viewportWidthPixels,
                                  double viewportHeightPixels) const {
    return projectionMatrix(viewportWidthPixels, viewportHeightPixels) * viewMatrix();
}

Frustum Camera::frustum(double viewportWidthPixels,
                        double viewportHeightPixels) const {
    return Frustum::fromViewProjection(
        viewProjectionMatrix(viewportWidthPixels, viewportHeightPixels));
}

Ray Camera::getPickRay(double screenXPixels,
                       double screenYPixels,
                       double viewportWidthPixels,
                       double viewportHeightPixels) const {
    if (viewportWidthPixels < kMinViewportPixels || viewportHeightPixels < kMinViewportPixels) {
        throw std::invalid_argument("Camera viewport dimensions must be positive pixels.");
    }

    const double ndcX = (2.0 * screenXPixels / viewportWidthPixels) - 1.0;
    const double ndcY = 1.0 - (2.0 * screenYPixels / viewportHeightPixels);

    const glm::dmat4 inverseViewProjection =
        glm::inverse(viewProjectionMatrix(viewportWidthPixels, viewportHeightPixels).raw());

    // Reverse-Z depth [0,1]: near(1m) → z_ndc=1, far(1e12) → z_ndc=0.
    const glm::dvec4 nearClip(ndcX, ndcY, 1.0, 1.0);
    const glm::dvec4 farClip(ndcX, ndcY, 0.0, 1.0);

    glm::dvec4 nearWorld = inverseViewProjection * nearClip;
    glm::dvec4 farWorld = inverseViewProjection * farClip;
    nearWorld /= nearWorld.w;
    farWorld /= farWorld.w;

    return Ray(Vec3(glm::dvec3(nearWorld)), Vec3(glm::dvec3(farWorld - nearWorld)));
}

double Camera::getHeight() const {
    const auto& wgs84 = Ellipsoid::WGS84();
    return wgs84.cartesianToCartographic(position_).height();
}

void Camera::getNormalMatrix(float out[9]) const {
    // 法线矩阵 = viewMatrix 的左上 3×3（旋转部分）
    // viewMatrix 是 column-major，取前三列的前三个元素
    auto vm = viewMatrix().raw();
    const double* m = glm::value_ptr(vm);
    // column-major: col0 = [m[0],m[1],m[2]], col1 = [m[4],m[5],m[6]], col2 = [m[8],m[9],m[10]]
    out[0] = static_cast<float>(m[0]);  // col0.x
    out[1] = static_cast<float>(m[1]);  // col0.y
    out[2] = static_cast<float>(m[2]);  // col0.z
    out[3] = static_cast<float>(m[4]);  // col1.x
    out[4] = static_cast<float>(m[5]);  // col1.y
    out[5] = static_cast<float>(m[6]);  // col1.z
    out[6] = static_cast<float>(m[8]);  // col2.x
    out[7] = static_cast<float>(m[9]);  // col2.y
    out[8] = static_cast<float>(m[10]); // col2.z
}

void Camera::setOrientation(const Vec3& direction, const Vec3& up) {
    if (direction.lengthSquared() <= 0.0 || up.lengthSquared() <= 0.0) {
        throw std::invalid_argument("Camera direction and up vectors must be non-zero.");
    }

    direction_ = direction.normalized();
    right_ = direction_.cross(up).normalized();
    up_ = right_.cross(direction_).normalized();
}

} // namespace earth_engine
