#include "CameraController.h"
#include "../scene/Camera.h"
#include "../core/math/Vec3.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

glm::dvec3 cartographicNormal(double lngDeg, double latDeg) {
    const double lng = glm::radians(lngDeg);
    const double lat = glm::radians(latDeg);
    const double cosLat = std::cos(lat);
    return glm::normalize(glm::dvec3(
        cosLat * std::cos(lng),
        cosLat * std::sin(lng),
        std::sin(lat)));
}

glm::dquat defaultViewRotation() {
    // Start over East Asia so XYZ Web Mercator imagery can be inspected without
    // the Web Mercator polar cutoff dominating the first frame.
    const glm::dvec3 baseViewDir(0.0, 0.0, 1.0);
    const glm::dvec3 desiredEye = cartographicNormal(105.0, 35.0);
    const glm::dvec3 desiredViewDir = -desiredEye;
    const double dot = std::clamp(glm::dot(baseViewDir, desiredViewDir), -1.0, 1.0);
    if (dot > 0.999999) return glm::dquat(1.0, 0.0, 0.0, 0.0);
    if (dot < -0.999999) return glm::angleAxis(glm::pi<double>(), glm::dvec3(0.0, 1.0, 0.0));

    const glm::dvec3 axis = glm::normalize(glm::cross(baseViewDir, desiredViewDir));
    const double angle = std::acos(dot);
    return glm::angleAxis(angle, axis);
}

} // namespace

CameraController::CameraController(Camera* camera)
    : camera_(camera),
      rotation_(defaultViewRotation()) {
    // 初始相机位置：沿 +Z 轴，距离地球中心 distance_ 个地球半径外
    update(0.0);
}

void CameraController::setViewport(int widthPixels, int heightPixels) {
    viewportWidth_ = std::max(1, widthPixels);
    viewportHeight_ = std::max(1, heightPixels);
}

void CameraController::onDragStart(float xPixels, float yPixels, double timestamp) {
    dragging_ = true;
    dragStartX_ = xPixels;
    dragStartY_ = yPixels;
    dragLastX_ = xPixels;
    dragLastY_ = yPixels;
    inertiaAngularVelocity_ = 0.0;
    lastDragTimestamp_ = timestamp;
}

void CameraController::onDragMove(float xPixels, float yPixels, double timestamp) {
    if (!dragging_) return;

    orbit(dragLastX_, dragLastY_, xPixels, yPixels, timestamp);

    dragLastX_ = xPixels;
    dragLastY_ = yPixels;
}

void CameraController::onDragEnd() {
    if (!dragging_) return;
    dragging_ = false;
    // 惯性参数由 orbit() 中的最后一次调用设置
}

void CameraController::onPinch(float scale) {
    if (scale <= 0.0f) return;

    // 缩放因子接近 1.0 表示手势结束，重置基准
    if (std::abs(scale - 1.0f) < 0.004f) {
        pinching_ = false;
        pinchBaseDistance_ = distance_;
        return;
    }

    if (!pinching_) {
        pinchBaseDistance_ = distance_;
        pinching_ = true;
    }

    constexpr float kMinDistance = 2.4f;
    constexpr float kMaxDistance = 12.0f;
    inertiaAngularVelocity_ = 0.0;

    distance_ = std::clamp(pinchBaseDistance_ / scale, kMinDistance, kMaxDistance);
}

void CameraController::update(double deltaSeconds) {
    // 惯性衰减
    if (!dragging_ && inertiaAngularVelocity_ > 0.0001 && deltaSeconds > 0.0) {
        constexpr double kInertiaDampingPerSecond = 3.0;
        double angle = inertiaAngularVelocity_ * deltaSeconds;
        glm::dquat delta = glm::angleAxis(angle, inertiaAxis_);
        rotation_ = glm::normalize(delta * rotation_);
        inertiaAngularVelocity_ *= std::exp(-kInertiaDampingPerSecond * deltaSeconds);
    }

    // 计算相机在 ECEF 空间中的位置
    // 旋转四元数作用于相机方向：相机沿 -Z 看地球，旋转改变朝向
    constexpr double kEarthRadius = 6378137.0;
    const double cameraDist = static_cast<double>(distance_) * kEarthRadius;

    // 默认视线方向：沿 +Z（从前方看地球）
    glm::dvec3 viewDir(0.0, 0.0, 1.0);
    glm::dvec3 upDir(0.0, 1.0, 0.0);

    // 应用轨道旋转
    glm::dvec3 rotatedDir = rotation_ * viewDir;
    glm::dvec3 rotatedUp = rotation_ * upDir;

    // 相机位置 = 地球中心 + 视线反方向 × 距离
    glm::dvec3 eyePos = -rotatedDir * cameraDist;

    // 更新 Camera
    camera_->lookAt(
        Vec3(eyePos.x, eyePos.y, eyePos.z),    // position
        Vec3::zero(),                            // target (earth center)
        Vec3(rotatedUp.x, rotatedUp.y, rotatedUp.z)  // up
    );
}

void CameraController::setDistance(float earthRadii) {
    distance_ = std::clamp(earthRadii, 2.4f, 12.0f);
    inertiaAngularVelocity_ = 0.0;
}

void CameraController::setRotation(const glm::dquat& q) {
    rotation_ = q;
    inertiaAngularVelocity_ = 0.0;
}

// ============================================================
// Private helpers
// ============================================================

float CameraController::projectedGlobeRadiusPixels() const {
    // 地球投影半径，使用 Camera 的实际 FOV
    float fov = static_cast<float>(camera_->verticalFovRadians());
    float focalLengthPixels = (static_cast<float>(viewportHeight_) * 0.5f) /
                               std::tan(fov * 0.5f);
    return std::max(1.0f, focalLengthPixels / distance_);
}

glm::vec3 CameraController::mapToArcball(float xPixels, float yPixels) const {
    float radius = projectedGlobeRadiusPixels();
    float nx = (xPixels - static_cast<float>(viewportWidth_) * 0.5f) / radius;
    float ny = (static_cast<float>(viewportHeight_) * 0.5f - yPixels) / radius;
    float lengthSq = nx * nx + ny * ny;

    if (lengthSq <= 1.0f) {
        return glm::normalize(glm::vec3(nx, ny, std::sqrt(1.0f - lengthSq)));
    }
    return glm::normalize(glm::vec3(nx, ny, 0.0f));
}

void CameraController::orbit(float startX, float startY, float endX, float endY,
                              double timestamp) {
    glm::vec3 from = mapToArcball(startX, startY);
    glm::vec3 to = mapToArcball(endX, endY);

    glm::vec3 axis = glm::cross(from, to);
    float axisLength = glm::length(axis);
    if (axisLength < 1e-5f) {
        return;
    }

    float dot = std::clamp(glm::dot(from, to), -1.0f, 1.0f);
    float angle = std::atan2(axisLength, dot);

    glm::dvec3 normalizedAxis(axis.x, axis.y, axis.z);
    normalizedAxis = glm::normalize(normalizedAxis);
    glm::dquat delta = glm::angleAxis(static_cast<double>(angle), normalizedAxis);
    rotation_ = glm::normalize(delta * rotation_);

    // 更新惯性（使用事件时间戳，而非渲染时钟）
    constexpr double kMaxInertiaAngularVelocity = 5.0;
    constexpr double kVelocitySmoothing = 0.35;
    double dt = timestamp - lastDragTimestamp_;
    if (dt > 0.0 && dt < 0.25) {
        double instantaneousVelocity = std::min(static_cast<double>(angle) / dt,
                                                kMaxInertiaAngularVelocity);
        inertiaAxis_ = normalizedAxis;
        inertiaAngularVelocity_ =
            inertiaAngularVelocity_ * (1.0 - kVelocitySmoothing) +
            instantaneousVelocity * kVelocitySmoothing;
    }
    lastDragTimestamp_ = timestamp;
}

} // namespace earth_engine
