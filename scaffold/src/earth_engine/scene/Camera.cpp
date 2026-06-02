#include "Camera.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
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
      nearPlaneMeters_(1.0),
      farPlaneMeters_(200000000.0) {}  // 200M — covers globe at 7 earth radii

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

    const double aspect = viewportWidthPixels / viewportHeightPixels;
    return Mat4(glm::perspective(verticalFovRadians_,
                                 aspect,
                                 nearPlaneMeters_,
                                 farPlaneMeters_));
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

    const glm::dvec4 nearClip(ndcX, ndcY, -1.0, 1.0);
    const glm::dvec4 farClip(ndcX, ndcY, 1.0, 1.0);

    glm::dvec4 nearWorld = inverseViewProjection * nearClip;
    glm::dvec4 farWorld = inverseViewProjection * farClip;
    nearWorld /= nearWorld.w;
    farWorld /= farWorld.w;

    return Ray(Vec3(glm::dvec3(nearWorld)), Vec3(glm::dvec3(farWorld - nearWorld)));
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
