#include "CameraController.h"
#include "../scene/Camera.h"
#include "../core/math/Ray.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <utility>

namespace earth_engine {

namespace {

constexpr double kMaxInertiaAngularVelocityRadPerSec = 5.0;
constexpr double kInertiaDampingPerSecond = 3.0;
constexpr double kVelocitySmoothing = 0.35;
constexpr double kEarthRadiusMeters = 6378137.0;
constexpr double kOpenGlobusMinAltitudeMeters = 1.0;
constexpr float kMinDistanceEarthRadii =
    static_cast<float>((kEarthRadiusMeters + kOpenGlobusMinAltitudeMeters) /
                       kEarthRadiusMeters);
constexpr float kMaxDistanceEarthRadii = 30.0f;
constexpr double kOpenGlobusTouchJerkLimit = 0.3;
constexpr double kOpenGlobusTouchInertia = 0.007;
constexpr double kOpenGlobusTouchMinSlope = 0.1;

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

glm::dvec3 clampEyeToOpenGlobusMinAltitude(const glm::dvec3& eye) {
    const double minRadius = kEarthRadiusMeters + kOpenGlobusMinAltitudeMeters;
    const double radius = glm::length(eye);
    if (radius >= minRadius || radius < 1e-6) {
        return eye;
    }
    return glm::normalize(eye) * minRadius;
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

void CameraController::setSurfacePicker(SurfacePicker picker) {
    surfacePicker_ = std::move(picker);
}

void CameraController::onDragStart(float xPixels, float yPixels, double timestamp) {
    update(0.0);
    orbitMode_ = false;
    dragging_ = grabSurfacePoint(xPixels, yPixels);
    dragStartX_ = xPixels;
    dragStartY_ = yPixels;
    dragLastX_ = xPixels;
    dragLastY_ = yPixels;
    dragStartEye_ = camera_->position();
    inertiaAngularVelocity_ = 0.0;
    touchInertiaScale_ = 0.0;
    touchInertiaRotation_ = glm::dquat(1.0, 0.0, 0.0, 0.0);
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
                                      float centerDeltaX,
                                      float centerDeltaY,
                                      double /*timestamp*/) {
    if (scale <= 0.0f) return;

    // Pinch starts/updates interrupt drag inertia. OpenGlobus does the same by
    // stopping qRot when touch mode changes.
    inertiaAngularVelocity_ = 0.0;
    touchInertiaScale_ = 0.0;
    touchInertiaRotation_ = glm::dquat(1.0, 0.0, 0.0, 0.0);
    dragging_ = false;
    hasGrabbedPoint_ = false;

    if (!pinching_) {
        pinching_ = true;
        update(0.0);
        orbitMode_ = false;

        Vec3 anchorPoint;
        grabbedRadiusMeters_ = kEarthRadiusMeters;
        hasPinchAnchor_ = pickSurfacePoint(centerX, centerY, anchorPoint);
        if (hasPinchAnchor_) {
            grabbedRadiusMeters_ = anchorPoint.length();
            pinchAnchorNormal_ = anchorPoint.normalized();
            Vec3 screenCenterPoint;
            if (pickSurfacePoint(static_cast<float>(viewportWidth_) * 0.5f,
                                 static_cast<float>(viewportHeight_) * 0.5f,
                                 screenCenterPoint)) {
                pinchEarthUpNormal_ = screenCenterPoint.normalized();
            } else {
                pinchEarthUpNormal_ = pinchAnchorNormal_;
            }
            pinchAnchorScreenX_ = centerX;
            pinchAnchorScreenY_ = centerY;
        }
    }

    inertiaAngularVelocity_ = 0.0;

    const double jerkMin = 1.0 - kOpenGlobusTouchJerkLimit;
    const double jerkMax = 1.0 + kOpenGlobusTouchJerkLimit;
    const double clampedScale = std::clamp(static_cast<double>(scale), jerkMin, jerkMax);

    if (hasPinchAnchor_) {
        Vec3 currentCenterPoint;
        grabbedRadiusMeters_ = kEarthRadiusMeters;
        if (!pickSurfacePoint(centerX, centerY, currentCenterPoint)) {
            currentCenterPoint = Vec3(pinchAnchorNormal_.raw() * grabbedRadiusMeters_);
        } else {
            grabbedRadiusMeters_ = currentCenterPoint.length();
            pinchAnchorNormal_ = currentCenterPoint.normalized();
        }

        const glm::dvec3 pointOnEarth = currentCenterPoint.raw();
        const double distanceToAnchor = camera_->position().distanceTo(currentCenterPoint);
        const double moveMeters = distanceToAnchor * (clampedScale - 1.0);
        glm::dvec3 nextEye =
            camera_->position().raw() +
            camera_->direction().raw() * moveMeters;
        nextEye = clampEyeToOpenGlobusMinAltitude(nextEye);
        const double nextDistanceRadii =
            glm::length(nextEye) / kEarthRadiusMeters;
        if (nextDistanceRadii <= kMaxDistanceEarthRadii) {
            camera_->setView(Vec3(nextEye), camera_->direction(), camera_->up());
            syncDistanceFromCamera();
        }

        if (std::abs(rotationRadians) > 1e-5f) {
            // OpenGlobus TouchNavigation:
            //   deltaAngle = curAngle - prevAngle
            //   cam.rotateAround(-deltaAngle, false, pointOnEarth, earthUp)
            // This controller stores the inverse orbit pose, so applying the
            // platform's signed screen delta directly matches that camera move.
            rotateCameraAroundPoint(
                pointOnEarth,
                pinchEarthUpNormal_.raw(),
                static_cast<double>(rotationRadians));
        }

        if (std::abs(centerDeltaX) > 0.5f || std::abs(centerDeltaY) > 0.5f) {
            update(0.0);
            const glm::dvec3 anchor = pinchAnchorNormal_.raw();
            const double focusDistanceMeters =
                std::max(kEarthRadiusMeters * 0.01,
                         camera_->position().distanceTo(Vec3(anchor * kEarthRadiusMeters)));
            const double cameraHeightMeters =
                std::max(0.0, camera_->position().length() - kEarthRadiusMeters);
            double sensitivity = (0.5 / focusDistanceMeters) *
                                 cameraHeightMeters *
                                 glm::pi<double>() / 180.0;
            if (sensitivity > 0.003) {
                sensitivity = 0.003;
            }

            if (std::abs(centerDeltaX) > 0.5f) {
                rotateCameraAroundPoint(
                    pointOnEarth,
                    pinchEarthUpNormal_.raw(),
                    sensitivity * static_cast<double>(centerDeltaX));
            }

            if (std::abs(centerDeltaY) > 0.5f) {
                rotateCameraVerticalAroundPoint(
                    pointOnEarth,
                    -sensitivity * static_cast<double>(centerDeltaY),
                    kOpenGlobusTouchMinSlope);
            }
        }
    } else {
        // 无有效 pinch anchor 时，沿视线方向缩放相机（与 OpenGlobus 行为一致）。
        // 不能仅设置 distance_，因为 orbitMode_ 已关闭，update() 不消费它。
        const double moveMeters =
            camera_->position().length() * (clampedScale - 1.0);
        glm::dvec3 nextEye =
            camera_->position().raw() +
            camera_->direction().raw() * moveMeters;
        nextEye = clampEyeToOpenGlobusMinAltitude(nextEye);
        if ((glm::length(nextEye) / kEarthRadiusMeters) <= kMaxDistanceEarthRadii) {
            camera_->setView(Vec3(nextEye), camera_->direction(), camera_->up());
            syncDistanceFromCamera();
        }
    }
}

void CameraController::onPinchEnd() {
    pinching_ = false;
    hasPinchAnchor_ = false;
    inertiaAngularVelocity_ = 0.0;
}

void CameraController::update(double deltaSeconds) {
    if (!dragging_ && touchInertiaScale_ > 0.0 && deltaSeconds > 0.0) {
        touchInertiaScale_ -= kOpenGlobusTouchInertia;
        if (touchInertiaScale_ <= 0.0) {
            touchInertiaScale_ = 0.0;
        } else {
            const double t = 1.0 - touchInertiaScale_ * touchInertiaScale_ * touchInertiaScale_;
            const glm::dquat delta = glm::normalize(glm::slerp(
                touchInertiaRotation_,
                glm::dquat(1.0, 0.0, 0.0, 0.0),
                t));
            if (std::abs(delta.x) > 1e-12 ||
                std::abs(delta.y) > 1e-12 ||
                std::abs(delta.z) > 1e-12) {
                applyCameraRotation(delta);
                // OpenGlobus: terrain collision check during touch inertia
                glm::dvec3 clampedEye = clampEyeToOpenGlobusMinAltitude(
                    camera_->position().raw());
                if (glm::length(clampedEye - camera_->position().raw()) > 1e-6) {
                    camera_->setView(Vec3(clampedEye), camera_->direction(),
                                     camera_->up());
                }
            } else {
                touchInertiaScale_ = 0.0;
            }
        }
    } else if (!dragging_ && inertiaAngularVelocity_ > 0.0001 && deltaSeconds > 0.0) {
        double angle = inertiaAngularVelocity_ * deltaSeconds;
        glm::dquat delta = glm::angleAxis(angle, inertiaAxis_);
        if (orbitMode_) {
            rotation_ = glm::normalize(delta * rotation_);
        } else {
            applyCameraRotation(delta);
            // OpenGlobus: terrain collision check during inertia
            glm::dvec3 clampedEye = clampEyeToOpenGlobusMinAltitude(
                camera_->position().raw());
            if (glm::length(clampedEye - camera_->position().raw()) > 1e-6) {
                camera_->setView(Vec3(clampedEye), camera_->direction(),
                                 camera_->up());
            }
        }
        inertiaAngularVelocity_ *= std::exp(-kInertiaDampingPerSecond * deltaSeconds);
    }

    if (!orbitMode_) {
        syncDistanceFromCamera();
        return;
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
    orbitMode_ = true;
    distance_ = std::clamp(
        earthRadii,
        kMinDistanceEarthRadii,
        kMaxDistanceEarthRadii);
    inertiaAngularVelocity_ = 0.0;
    touchInertiaScale_ = 0.0;
}

void CameraController::setRotation(const glm::dquat& q) {
    orbitMode_ = true;
    rotation_ = glm::normalize(q);
    inertiaAngularVelocity_ = 0.0;
    touchInertiaScale_ = 0.0;
}

void CameraController::viewDistance(const Vec3& targetWorld, double distanceMeters) {
    const double maxDistanceMeters = kMaxDistanceEarthRadii * kEarthRadiusMeters;
    const double clampedDistance = std::clamp(
        distanceMeters,
        kOpenGlobusMinAltitudeMeters,
        maxDistanceMeters);

    glm::dvec3 away = camera_->position().raw() - targetWorld.raw();
    if (glm::length(away) < 1e-6) {
        away = -camera_->direction().raw();
    }
    if (glm::length(away) < 1e-6) {
        away = glm::normalize(targetWorld.raw());
    }

    const glm::dvec3 eye = targetWorld.raw() + glm::normalize(away) * clampedDistance;
    camera_->lookAt(Vec3(eye), targetWorld, camera_->up());
    orbitMode_ = false;
    inertiaAngularVelocity_ = 0.0;
    touchInertiaScale_ = 0.0;
    syncDistanceFromCamera();
}

void CameraController::applyRotationAroundAxis(const glm::dvec3& axis, double angle) {
    if (glm::length(axis) < 1e-10 || std::abs(angle) < 1e-12) {
        return;
    }
    const glm::dquat delta = glm::angleAxis(angle, glm::normalize(axis));
    if (orbitMode_) {
        rotation_ = glm::normalize(delta * rotation_);
    } else {
        applyCameraRotation(delta);
    }
}

void CameraController::applyCameraRotation(const glm::dquat& delta) {
    const glm::dvec3 eye = delta * camera_->position().raw();
    const glm::dvec3 direction = delta * camera_->direction().raw();
    const glm::dvec3 up = delta * camera_->up().raw();
    camera_->setView(Vec3(eye), Vec3(direction), Vec3(up));
    rotation_ = glm::normalize(delta * rotation_);
    syncDistanceFromCamera();
}

void CameraController::rotateCameraAroundPoint(const glm::dvec3& center,
                                               const glm::dvec3& axis,
                                               double angle) {
    if (glm::length(axis) < 1e-10 || std::abs(angle) < 1e-12) {
        return;
    }
    const glm::dquat delta = glm::angleAxis(angle, glm::normalize(axis));
    const glm::dvec3 eye = center + delta * (camera_->position().raw() - center);
    const glm::dvec3 direction = delta * camera_->direction().raw();
    const glm::dvec3 up = delta * camera_->up().raw();
    camera_->setView(Vec3(eye), Vec3(direction), Vec3(up));
    rotation_ = glm::normalize(delta * rotation_);
    syncDistanceFromCamera();
}

void CameraController::rotateCameraVerticalAroundPoint(const glm::dvec3& center,
                                                       double angle,
                                                       double minSlope) {
    const glm::dvec3 axis = camera_->right().raw();
    if (glm::length(axis) < 1e-10 || std::abs(angle) < 1e-12) {
        return;
    }

    const glm::dvec3 currentEyeNorm = glm::normalize(camera_->position().raw());
    const double currentSlope = glm::dot(-camera_->direction().raw(), currentEyeNorm);

    const glm::dquat delta = glm::angleAxis(angle, glm::normalize(axis));
    const glm::dvec3 nextEye = center + delta * (camera_->position().raw() - center);
    const glm::dvec3 nextDirection = delta * camera_->direction().raw();
    const glm::dvec3 nextUp = delta * camera_->up().raw();

    const glm::dvec3 nextEyeNorm = glm::normalize(nextEye);
    const double nextSlope = glm::dot(-nextDirection, nextEyeNorm);
    if (glm::dot(nextUp, nextEyeNorm) <= 0.0) {
        return;
    }

    if (minSlope > 0.0) {
        const double dSlope = nextSlope - currentSlope;
        if (nextSlope < minSlope && dSlope < 0.0) {
            return;
        }

        const bool canApply =
            (nextSlope > 0.1 && glm::dot(nextUp, nextEyeNorm) > 0.0) ||
            currentSlope <= 0.1 ||
            glm::dot(camera_->up().raw(), currentEyeNorm) <= 0.0;
        if (!canApply) {
            return;
        }
    }

    camera_->setView(Vec3(nextEye), Vec3(nextDirection), Vec3(nextUp));
    rotation_ = glm::normalize(delta * rotation_);
    syncDistanceFromCamera();
}

void CameraController::syncDistanceFromCamera() {
    distance_ = static_cast<float>(camera_->position().length() / kEarthRadiusMeters);
}

void CameraController::keepAnchorAtScreenPoint(const Vec3& anchorNormal,
                                               float xPixels,
                                               float yPixels) {
    Vec3 screenPointOnSphere;
    const Ray ray = camera_->getPickRay(
        static_cast<double>(xPixels),
        static_cast<double>(yPixels),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    if (!intersectGrabSphere(ray, screenPointOnSphere)) {
        return;
    }

    const glm::dvec3 from = screenPointOnSphere.normalized().raw();
    const glm::dvec3 to = anchorNormal.raw();
    glm::dvec3 axis = glm::cross(from, to);
    const double axisLength = glm::length(axis);
    if (axisLength < 1e-10) {
        return;
    }

    const double dot = std::clamp(glm::dot(from, to), -1.0, 1.0);
    const double angle = std::atan2(axisLength, dot);
    axis /= axisLength;
    applyRotationAroundAxis(axis, angle);
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

bool CameraController::pickSurfacePoint(float xPixels, float yPixels, Vec3& outPoint) const {
    if (surfacePicker_ && surfacePicker_(xPixels, yPixels, outPoint)) {
        return true;
    }

    const Ray ray = camera_->getPickRay(
        static_cast<double>(xPixels),
        static_cast<double>(yPixels),
        static_cast<double>(viewportWidth_),
        static_cast<double>(viewportHeight_));
    return intersectGrabSphere(ray, outPoint);
}

bool CameraController::intersectPlane(const Ray& ray,
                                      const glm::dvec3& planePoint,
                                      const glm::dvec3& planeNormal,
                                      glm::dvec3& outPoint) const {
    const double denom = glm::dot(ray.direction().raw(), planeNormal);
    if (std::abs(denom) < 1e-12) {
        return false;
    }
    const double t = glm::dot(planePoint - ray.origin().raw(), planeNormal) / denom;
    if (t <= 0.0) {
        return false;
    }
    outPoint = ray.pointAt(t).raw();
    return true;
}

bool CameraController::grabSurfacePoint(float xPixels, float yPixels) {
    grabbedRadiusMeters_ = kEarthRadiusMeters;

    Vec3 grabbedPoint;
    if (!pickSurfacePoint(xPixels, yPixels, grabbedPoint)) {
        hasGrabbedPoint_ = false;
        return false;
    }

    grabbedRadiusMeters_ = grabbedPoint.length();
    grabbedPoint_ = grabbedPoint;
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
    if (!pickSurfacePoint(xPixels, yPixels, targetPoint)) {
        return;
    }

    const glm::dvec3 eyeNorm = glm::normalize(camera_->position().raw());
    const double slope = glm::dot(-camera_->direction().raw(), eyeNorm);
    if (slope <= 0.2) {
        const glm::dvec3 p0 = grabbedPoint_.raw();
        const glm::dvec3 planeNormal =
            glm::normalize(glm::cross(camera_->up().raw(), grabbedNormal_.raw()));
        glm::dvec3 planeHit;
        if (intersectPlane(ray, p0, planeNormal, planeHit)) {
            glm::dvec3 newEye = dragStartEye_.raw() - (planeHit - p0);
            newEye = clampEyeToOpenGlobusMinAltitude(newEye);
            camera_->setView(Vec3(newEye), camera_->direction(), camera_->up());
            syncDistanceFromCamera();
            touchInertiaScale_ = 0.0;
        }
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
    applyCameraRotation(delta);
    // OpenGlobus: cam.checkTerrainCollision() after every camera move
    {
        glm::dvec3 clampedEye = clampEyeToOpenGlobusMinAltitude(
            camera_->position().raw());
        if (glm::length(clampedEye - camera_->position().raw()) > 1e-6) {
            camera_->setView(Vec3(clampedEye), camera_->direction(),
                             camera_->up());
            syncDistanceFromCamera();
        }
    }
    touchInertiaRotation_ = delta;
    touchInertiaScale_ = 1.0;

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
