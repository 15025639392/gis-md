#include "CameraController.h"
#include "../debug/PlatformLog.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
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
// Keep a small visual floor to avoid clipping the ellipsoid surface near ground.
constexpr double kMinAltitudeMeters = 50.0;
// Real-world terrain never exceeds ~8849 m (Everest); use a margin above it.
// When the eye is already higher than this plus the altitude floor, the terrain
// clamp can never trigger, so the (expensive, per-frame) terrain height query is
// pure waste — skip it. This is what keeps panning/zooming smooth at altitude:
// the query scans every loaded tile's mesh triangles (each with ECEF→geodetic
// round-trips) and otherwise costs >150 ms/frame.
constexpr double kMaxTerrainHeightMeters = 9000.0;
constexpr float kMinDistanceEarthRadii =
    static_cast<float>((kEarthRadiusMeters + kMinAltitudeMeters) /
                       kEarthRadiusMeters);
constexpr float kMaxDistanceEarthRadii = 30.0f;
constexpr double kTouchJerkLimit = 0.3;
constexpr double kTouchMinSlope = 0.1;
constexpr double kPinchIntentThresholdPixels = 4.0;
constexpr double kPinchTiltThresholdPixels = 10.0;
constexpr double kPinchTiltRadiansPerPixel = 0.0015;
constexpr double kPinchTiltMaxStepRadians = 0.08;
// 仅作噪声地板：足够小以让缓慢拧动逐帧响应（旧值 0.003rad≈0.17°/事件会把
// 刻意的慢速旋转整段吞掉，手感 steppy），又足够大以滤除双指角度传感抖动。
constexpr double kPinchRotateThresholdRadians = 0.0003;
constexpr double kPinchAnchorFollow = 0.12;

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

glm::dvec3 clampEyeToMinAltitude(const glm::dvec3& eye,
                                 const CameraController::TerrainHeightFunc& terrainFunc) {
    if (glm::length(eye) < 1e-6) return eye;

    const auto& ellipsoid = Ellipsoid::WGS84();
    const Vec3 eyeVec(eye);
    const Cartographic cart = ellipsoid.cartesianToCartographic(eyeVec);

    // Fast path: already above the tallest possible terrain + floor, so the
    // clamp below can never change the eye. Skip the costly terrain query.
    if (cart.height() >= kMaxTerrainHeightMeters + kMinAltitudeMeters) {
        return eye;
    }

    const Vec3 surface = ellipsoid.projectToSurface(eyeVec);
    const Vec3 normal = ellipsoid.geodeticSurfaceNormal(surface);

    double terrainHeight = 0.0;
    if (terrainFunc) {
        terrainHeight = terrainFunc(surface);
    }

    const double minHeight = std::max(terrainHeight, 0.0) +
                             kMinAltitudeMeters;

    if (cart.height() >= minHeight) return eye;
    return (surface + normal * minHeight).raw();
}

} // namespace

CameraController::CameraController(Camera* camera)
    : camera_(camera),
      rotation_(defaultViewRotation()) {
    // Default to Chongqing area for testing
    const auto& e = Ellipsoid::WGS84();
    auto target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.508, 29.617, 0.0));
    auto eye = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.508, 29.617, 1500.0));
    camera_->lookAt(eye, target, e.geodeticSurfaceNormal(target));
    orbitMode_ = false;
}

void CameraController::setViewport(int widthPixels, int heightPixels) {
    viewportWidth_ = std::max(1, widthPixels);
    viewportHeight_ = std::max(1, heightPixels);
}

void CameraController::setSurfacePicker(SurfacePicker picker) {
    surfacePicker_ = std::move(picker);
}

void CameraController::setTerrainHeightFunc(TerrainHeightFunc func) {
    terrainHeightFunc_ = std::move(func);
}

void CameraController::onDragStart(float xPixels, float yPixels, double timestamp) {
    update(0.0);
    orbitMode_ = false;
    // 抓取起始地表点；miss（按在地平线外/空白处）不再放弃整段拖拽，
    // 而是进入 spin 回退。grabSurfacePoint 内部已设置 hasGrabbedPoint_。
    grabSurfacePoint(xPixels, yPixels);
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

    // Pinch starts/updates interrupt drag inertia; mixed inertias feel unstable.
    inertiaAngularVelocity_ = 0.0;
    dragging_ = false;
    hasGrabbedPoint_ = false;

    if (!pinching_) {
        pinching_ = true;
        update(0.0);
        orbitMode_ = false;

        Vec3 anchorPoint;
        grabbedRadiusMeters_ = kEarthRadiusMeters;
        hasPinchAnchor_ = pickSurfacePoint(centerX, centerY, anchorPoint);
        platformLog(LogLevel::Info, "CameraCtrl",
            "pinchStart hasAnchor=%d center=(%.0f,%.0f)",
            hasPinchAnchor_, centerX, centerY);
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

    const double jerkMin = 1.0 - kTouchJerkLimit;
    const double jerkMax = 1.0 + kTouchJerkLimit;
    const double clampedScale = std::clamp(static_cast<double>(scale), jerkMin, jerkMax);

    if (hasPinchAnchor_) {
        const glm::dvec3 pointOnEarth =
            pinchAnchorNormal_.raw() * grabbedRadiusMeters_;
        const Vec3 anchorPoint(pointOnEarth);
        const double distanceToAnchor = camera_->position().distanceTo(anchorPoint);
        const double moveMeters = distanceToAnchor * (clampedScale - 1.0);
        glm::dvec3 nextEye =
            camera_->position().raw() +
            camera_->direction().raw() * moveMeters;
        nextEye = clampEyeAltitude(nextEye);
        const double nextDistanceRadii =
            glm::length(nextEye) / kEarthRadiusMeters;
        if (nextDistanceRadii <= kMaxDistanceEarthRadii) {
            camera_->setView(Vec3(nextEye), camera_->direction(), camera_->up());
            syncDistanceFromCamera();
        }

        const bool rotateIntent =
            std::abs(rotationRadians) > kPinchRotateThresholdRadians;
        if (rotateIntent) {
            rotateCameraAroundPoint(
                pointOnEarth,
                pinchEarthUpNormal_.raw(),
                static_cast<double>(rotationRadians));
        }

        const double absCenterDx = std::abs(static_cast<double>(centerDeltaX));
        const double absCenterDy = std::abs(static_cast<double>(centerDeltaY));
        const double scaleIntent = std::abs(std::log(std::max(clampedScale, 1e-6)));
        const bool centerIntent =
            absCenterDx > kPinchIntentThresholdPixels ||
            absCenterDy > kPinchIntentThresholdPixels;
        const bool scaleDominant = scaleIntent > 0.015 &&
            scaleIntent * 900.0 > std::max(absCenterDx, absCenterDy);
        const bool zoomIntent = scaleIntent > 0.001;
        const bool tiltIntent =
            absCenterDy > kPinchTiltThresholdPixels &&
            absCenterDy > absCenterDx * 1.35;

        if (tiltIntent && !scaleDominant) {
            update(0.0);
            const double tiltAngle = std::clamp(
                -kPinchTiltRadiansPerPixel * static_cast<double>(centerDeltaY),
                -kPinchTiltMaxStepRadians,
                kPinchTiltMaxStepRadians);
            rotateCameraVerticalAroundPoint(
                pointOnEarth,
                tiltAngle,
                kTouchMinSlope);
        }

        if (zoomIntent || rotateIntent) {
            keepAnchorAtScreenPoint(pinchAnchorNormal_, centerX, centerY);
        } else if (tiltIntent && !scaleDominant) {
            keepAnchorAtScreenPoint(
                pinchAnchorNormal_,
                pinchAnchorScreenX_,
                pinchAnchorScreenY_);
        }

        // 倾斜时 centerDeltaY 大 → centerIntent 为真，但那是 pitch 意图不是 pan；
        // 若在此把锚点朝手指方向混合，会把锚点推离原位、地图看着像被平移
        // （真机可视化实测偏 ~17px）。故倾斜时不做质心跟随，锚点保持为 pitch 支点。
        if (centerIntent && !scaleDominant && !tiltIntent) {
            Vec3 currentCenterPoint;
            if (pickSurfacePoint(centerX, centerY, currentCenterPoint)) {
                const glm::dvec3 blended = glm::normalize(
                    glm::mix(pinchAnchorNormal_.raw(),
                             currentCenterPoint.normalized().raw(),
                             kPinchAnchorFollow));
                pinchAnchorNormal_ = Vec3(blended);
                grabbedRadiusMeters_ =
                    grabbedRadiusMeters_ * (1.0 - kPinchAnchorFollow) +
                    currentCenterPoint.length() * kPinchAnchorFollow;
            }
        }
    } else {
        // 无有效 pinch anchor 时，沿视线方向缩放相机。
        // 不能仅设置 distance_，因为 orbitMode_ 已关闭，update() 不消费它。
        const double moveMeters =
            camera_->position().length() * (clampedScale - 1.0);
        glm::dvec3 nextEye =
            camera_->position().raw() +
            camera_->direction().raw() * moveMeters;
        nextEye = clampEyeAltitude(nextEye);
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
    // Flick inertia is velocity-based only: the released angular velocity
    // (rad/s, dt-scaled and exponentially damped below) continues the pan.
    // The previous quaternion "touch inertia" re-applied ~s^3 of the LAST
    // drag event's full rotation EVERY FRAME (~36x the event delta in total,
    // frame-rate dependent), which flung the camera hundreds of kilometers
    // after one swipe when input events were coalesced under load.
    if (!dragging_ && inertiaAngularVelocity_ > 0.0001 && deltaSeconds > 0.0) {
        double angle = inertiaAngularVelocity_ * deltaSeconds;
        glm::dquat delta = glm::angleAxis(angle, inertiaAxis_);
        if (orbitMode_) {
            rotation_ = glm::normalize(delta * rotation_);
        } else {
            applyCameraRotation(delta);
            glm::dvec3 clampedEye = clampEyeAltitude(
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
}

void CameraController::setRotation(const glm::dquat& q) {
    orbitMode_ = true;
    rotation_ = glm::normalize(q);
    inertiaAngularVelocity_ = 0.0;
}

void CameraController::viewDistance(const Vec3& targetWorld, double distanceMeters) {
    const double maxDistanceMeters = kMaxDistanceEarthRadii * kEarthRadiusMeters;
    const double clampedDistance = std::clamp(
        distanceMeters,
        kMinAltitudeMeters,
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

glm::dvec3 CameraController::clampEyeAltitude(const glm::dvec3& eye) const {
    return clampEyeToMinAltitude(eye, terrainHeightFunc_);
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
    glm::dquat delta;

    Vec3 targetPoint;
    const bool anchorValid =
        hasGrabbedPoint_ && pickSurfacePoint(xPixels, yPixels, targetPoint);

    if (anchorValid) {
        // Anchor pan：旋转让被抓地表点跟随手指（起点/当前都命中球面）。
        const glm::dvec3 from = targetPoint.normalized().raw();
        const glm::dvec3 to = grabbedNormal_.raw();
        const glm::dvec3 axis = glm::cross(from, to);
        const double axisLength = glm::length(axis);
        if (axisLength < 1e-10) {
            return;
        }
        const double dot = std::clamp(glm::dot(from, to), -1.0, 1.0);
        const double angle = std::atan2(axisLength, dot);
        delta = glm::angleAxis(angle, axis / axisLength);
    } else {
        // Spin 回退（等价 cesium _spin3D）：手指在地平线外/空白处，没有
        // 地表点可锚定，改用屏幕像素位移按"转台"方式绕地心转相机。一旦
        // miss 就 latch 到 spin 直到抬手——避免近球缘 pan<->spin 每帧抖动，
        // 以及重入球面时把过期锚点猛拉回指下的跳变。
        hasGrabbedPoint_ = false;

        const double dx = static_cast<double>(xPixels) - dragLastX_;
        const double dy = static_cast<double>(yPixels) - dragLastY_;
        if (std::abs(dx) < 1e-6 && std::abs(dy) < 1e-6) {
            return;
        }
        // 屏幕中心处每像素约对应的角度，给出接近 1:1 的转台手感。
        // 水平/垂直每像素角度相同（aspect 抵消），故统一用 fov/height。
        const double radPerPixel =
            camera_->verticalFovRadians() /
            static_cast<double>(std::max(1, viewportHeight_));
        // 手指右移 → 世界右转（绕屏幕竖轴=camera up）；
        // 手指下移 → 世界下转（绕屏幕横轴=camera right）。
        const glm::dquat yaw =
            glm::angleAxis(-dx * radPerPixel, camera_->up().raw());
        const glm::dquat pitch =
            glm::angleAxis(-dy * radPerPixel, camera_->right().raw());
        delta = glm::normalize(yaw * pitch);
    }

    applyCameraRotation(delta);
    {
        glm::dvec3 clampedEye = clampEyeAltitude(
            camera_->position().raw());
        if (glm::length(clampedEye - camera_->position().raw()) > 1e-6) {
            camera_->setView(Vec3(clampedEye), camera_->direction(),
                             camera_->up());
            syncDistanceFromCamera();
        }
    }

    // 更新惯性（使用事件时间戳，而非渲染时钟）；spin 与 anchor 共用同一通道，
    // 故隔着地平线甩一下也能顺滑滑行。
    const double angle = glm::angle(delta);
    double dt = timestamp - lastDragTimestamp_;
    if (angle > 1e-9 && dt > 0.0 && dt < 0.25) {
        double instantaneousVelocity = std::min(angle / dt,
                                                kMaxInertiaAngularVelocityRadPerSec);
        inertiaAxis_ = glm::axis(delta);
        inertiaAngularVelocity_ =
            inertiaAngularVelocity_ * (1.0 - kVelocitySmoothing) +
            instantaneousVelocity * kVelocitySmoothing;
    }
    lastDragTimestamp_ = timestamp;
}

} // namespace earth_engine
