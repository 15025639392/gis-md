#include "CameraController.h"
#include "../scene/Camera.h"
#include "../core/math/Ray.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

constexpr double kMaxInertiaAngularVelocityRadPerSec = 5.0;
constexpr double kInertiaDampingPerSecond = 3.0;
constexpr double kVelocitySmoothing = 0.35;
constexpr double kEarthRadiusMeters = 6378137.0;

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
    update(0.0);
    dragging_ = grabSurfacePoint(xPixels, yPixels);
    dragStartX_ = xPixels;
    dragStartY_ = yPixels;
    dragLastX_ = xPixels;
    dragLastY_ = yPixels;
    inertiaAngularVelocity_ = 0.0;
    lastDragTimestamp_ = timestamp;
}

void CameraController::onDragMove(float xPixels, float yPixels, double timestamp) {
    if (!dragging_) return;

    applyAnchorDrag(xPixels, yPixels, timestamp);

    dragLastX_ = xPixels;
    dragLastY_ = yPixels;
}

void CameraController::onDragEnd() {
    if (!dragging_) return;
    dragging_ = false;
    hasGrabbedPoint_ = false;
    // 惯性参数由 orbit() 中的最后一次调用设置
}

void CameraController::onPinchGesture(float scale,
                                      float centerX,
                                      float centerY,
                                      float rotationRadians,
                                      float centerDeltaY,
                                      double /*timestamp*/) {
    if (scale <= 0.0f) return;

    // Pinch starts/updates interrupt drag inertia. OpenGlobus does the same by
    // stopping qRot when touch mode changes.
    inertiaAngularVelocity_ = 0.0;
    dragging_ = false;
    hasGrabbedPoint_ = false;

    if (!pinching_) {
        pinching_ = true;
        update(0.0);

        Vec3 earthUpPoint;
        const Ray centerRay = camera_->getPickRay(
            static_cast<double>(viewportWidth_) * 0.5,
            static_cast<double>(viewportHeight_) * 0.5,
            static_cast<double>(viewportWidth_),
            static_cast<double>(viewportHeight_));
        grabbedRadiusMeters_ = kEarthRadiusMeters;
        if (intersectGrabSphere(centerRay, earthUpPoint)) {
            pinchEarthUpNormal_ = earthUpPoint.normalized();
        }
    }

    Vec3 anchorPoint;
    const Ray middleRay = camera_->getPickRay(
        static_cast<double>(centerX),
        static_cast<double>(centerY),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    grabbedRadiusMeters_ = kEarthRadiusMeters;
    hasPinchAnchor_ = intersectGrabSphere(middleRay, anchorPoint);
    if (hasPinchAnchor_) {
        pinchAnchorNormal_ = anchorPoint.normalized();
    }

    constexpr float kMinDistance = 2.4f;
    constexpr float kMaxDistance = 12.0f;
    inertiaAngularVelocity_ = 0.0;

    distance_ = std::clamp(distance_ / scale, kMinDistance, kMaxDistance);

    if (hasPinchAnchor_) {
        if (std::abs(rotationRadians) > 1e-5f) {
            // OpenGlobus TouchNavigation:
            //   deltaAngle = curAngle - prevAngle
            //   cam.rotateAround(-deltaAngle, false, pointOnEarth, earthUp)
            // This controller stores the inverse orbit pose, so applying the
            // platform's signed screen delta directly matches that camera move.
            applyRotationAroundAxis(pinchEarthUpNormal_.raw(),
                                    static_cast<double>(rotationRadians));
        }

        if (std::abs(centerDeltaY) > 0.5f) {
            update(0.0);
            const glm::dvec3 anchor = pinchAnchorNormal_.raw();
            const glm::dvec3 right = camera_->right().raw();
            glm::dvec3 axis = glm::cross(right, anchor);
            if (glm::length(axis) > 1e-10) {
                axis = glm::normalize(axis);
                const double focusDistanceMeters =
                    std::max(kEarthRadiusMeters * 0.01,
                             camera_->position().distanceTo(Vec3(anchor * kEarthRadiusMeters)));
                double sensitivity = (0.5 / focusDistanceMeters) *
                                     glm::pi<double>() / 180.0;
                sensitivity = std::clamp(sensitivity, 0.003, 0.007);
                applyRotationAroundAxis(
                    axis,
                    sensitivity * static_cast<double>(centerDeltaY) * 0.75);
            }
        }
    }

    update(0.0);
}

void CameraController::onPinchEnd() {
    pinching_ = false;
    hasPinchAnchor_ = false;
    inertiaAngularVelocity_ = 0.0;
}

void CameraController::update(double deltaSeconds) {
    // 惯性衰减
    if (!dragging_ && inertiaAngularVelocity_ > 0.0001 && deltaSeconds > 0.0) {
        double angle = inertiaAngularVelocity_ * deltaSeconds;
        glm::dquat delta = glm::angleAxis(angle, inertiaAxis_);
        rotation_ = glm::normalize(delta * rotation_);
        inertiaAngularVelocity_ *= std::exp(-kInertiaDampingPerSecond * deltaSeconds);
    }

    // 计算相机在 ECEF 空间中的位置
    // 旋转四元数作用于相机方向：相机沿 -Z 看地球，旋转改变朝向
    const double cameraDist = static_cast<double>(distance_) * kEarthRadiusMeters;

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
    rotation_ = glm::normalize(q);
    inertiaAngularVelocity_ = 0.0;
}

void CameraController::applyRotationAroundAxis(const glm::dvec3& axis, double angle) {
    if (glm::length(axis) < 1e-10 || std::abs(angle) < 1e-12) {
        return;
    }
    const glm::dquat delta = glm::angleAxis(angle, glm::normalize(axis));
    rotation_ = glm::normalize(delta * rotation_);
}

// ============================================================
// Private helpers
// ============================================================

bool CameraController::intersectGrabSphere(const Ray& ray, Vec3& outPoint) const {
    const glm::dvec3 o = ray.origin().raw();
    const glm::dvec3 d = ray.direction().raw();
    const double b = 2.0 * glm::dot(o, d);
    const double c = glm::dot(o, o) - grabbedRadiusMeters_ * grabbedRadiusMeters_;
    const double disc = b * b - 4.0 * c;
    if (disc < 0.0) {
        return false;
    }

    const double sqrtDisc = std::sqrt(disc);
    const double t0 = (-b - sqrtDisc) * 0.5;
    const double t1 = (-b + sqrtDisc) * 0.5;
    const double t = t0 > 0.0 ? t0 : t1;
    if (t <= 0.0) {
        return false;
    }

    outPoint = ray.pointAt(t);
    return true;
}

bool CameraController::grabSurfacePoint(float xPixels, float yPixels) {
    grabbedRadiusMeters_ = kEarthRadiusMeters;

    Vec3 grabbedPoint;
    const Ray ray = camera_->getPickRay(
        static_cast<double>(xPixels),
        static_cast<double>(yPixels),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    if (!intersectGrabSphere(ray, grabbedPoint)) {
        hasGrabbedPoint_ = false;
        return false;
    }

    grabbedRadiusMeters_ = grabbedPoint.length();
    grabbedNormal_ = grabbedPoint.normalized();
    hasGrabbedPoint_ = true;
    return true;
}

void CameraController::applyAnchorDrag(float xPixels, float yPixels,
                                       double timestamp) {
    if (!hasGrabbedPoint_) {
        return;
    }

    Vec3 targetPoint;
    const Ray ray = camera_->getPickRay(
        static_cast<double>(xPixels),
        static_cast<double>(yPixels),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    if (!intersectGrabSphere(ray, targetPoint)) {
        return;
    }

    const glm::dvec3 from = targetPoint.normalized().raw();
    const glm::dvec3 to = grabbedNormal_.raw();
    const glm::dvec3 axis = glm::cross(from, to);
    const double axisLength = glm::length(axis);
    if (axisLength < 1e-10) {
        return;
    }

    const double dot = std::clamp(glm::dot(from, to), -1.0, 1.0);
    const double angle = std::atan2(axisLength, dot);

    const glm::dvec3 normalizedAxis = glm::normalize(axis);
    const glm::dquat delta = glm::angleAxis(angle, normalizedAxis);
    rotation_ = glm::normalize(delta * rotation_);
    update(0.0);

    // 更新惯性（使用事件时间戳，而非渲染时钟）
    double dt = timestamp - lastDragTimestamp_;
    if (dt > 0.0 && dt < 0.25) {
        double instantaneousVelocity = std::min(static_cast<double>(angle) / dt,
                                                kMaxInertiaAngularVelocityRadPerSec);
        inertiaAxis_ = normalizedAxis;
        inertiaAngularVelocity_ =
            inertiaAngularVelocity_ * (1.0 - kVelocitySmoothing) +
            instantaneousVelocity * kVelocitySmoothing;
    }
    lastDragTimestamp_ = timestamp;
}

} // namespace earth_engine
