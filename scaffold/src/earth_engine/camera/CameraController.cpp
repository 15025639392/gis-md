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

// Zoom 惯性：捏合松手后沿视线朝锚点继续滑一小段。刻意建模在"对数距离"空间
// （每秒的 ln(距离) 变化率），有三个好处：① 与高度无关，海拔 10km 和 100m
// 手感一致；② 指数逼近锚点、永不越过（distance*=exp(-r·dt)>0），从数学上排除
// 历史上那个 36× fling；③ 天然有界。速率上限 + 指数阻尼 + 低于地板即停。
constexpr double kZoomInertiaDampingPerSecond = 6.0;
constexpr double kMaxZoomInertiaLogRate = 6.0;   // |d(ln dist)/dt| 上限，滤抖动尖峰
constexpr double kMinZoomInertiaLogRate = 0.08;  // 低于此停止滑行

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
                                 const CameraController::TerrainHeightFunc& terrainFunc,
                                 double& lastKnownTerrainHeight) {
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

    // 无地形数据(未加载 / 无覆盖瓦片)时 terrainFunc 返回 nullopt——不能当海平面
    // 0 处理,否则相机会被允许下沉到未加载山体表面之下,瓦片加载后又被顶回(dip→
    // pop)。保守回退到上一次有效样本(初值 0),让下限稳定、消除突跳。有数据时更新
    // 缓存。无 terrainFunc(未接地形)时缓存恒 0 → 行为等价于旧的 bare-ellipsoid+50m。
    double terrainHeight = lastKnownTerrainHeight;
    if (terrainFunc) {
        const std::optional<double> sampled = terrainFunc(surface);
        if (sampled) {
            terrainHeight = *sampled;
            lastKnownTerrainHeight = *sampled;
        }
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
    hasZoomInertia_ = false;   // 拖拽打断 zoom 惯性滑行
    zoomInertiaLogRate_ = 0.0;
    lastDragTimestamp_ = timestamp;
    logGestureDiag("dragStart", xPixels, yPixels);
}

void CameraController::onDragMove(float xPixels, float yPixels, double timestamp) {
    if (!dragging_) return;

    applyAnchorDrag(xPixels, yPixels, timestamp);

    dragLastX_ = xPixels;
    dragLastY_ = yPixels;
    logGestureDiag("dragMove", xPixels, yPixels);
}

void CameraController::onDragEnd() {
    if (!dragging_) return;
    logGestureDiag("dragEnd", dragLastX_, dragLastY_);
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
                                      double timestamp) {
    if (scale <= 0.0f) return;

    // Pinch starts/updates interrupt drag inertia; mixed inertias feel unstable.
    inertiaAngularVelocity_ = 0.0;
    dragging_ = false;
    hasGrabbedPoint_ = false;

    const bool isPinchStartFrame = !pinching_;

    if (!pinching_) {
        pinching_ = true;
        update(0.0);
        orbitMode_ = false;
        // 新捏合打断上一段 zoom 惯性滑行，并重置速率累积。
        hasZoomInertia_ = false;
        zoomInertiaLogRate_ = 0.0;
        lastPinchTimestamp_ = timestamp;

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

        // 累积 zoom 惯性速率（对数距离空间，EMA 平滑）。clampedScale≈1 的纯
        // 旋转/倾斜帧会让速率自然衰减向 0，故只有真正在缩放才留下滑行动量。
        {
            const double dt = timestamp - lastPinchTimestamp_;
            if (dt > 0.0 && dt < 0.25) {
                const double instRate = std::clamp(
                    std::log(clampedScale) / dt,
                    -kMaxZoomInertiaLogRate, kMaxZoomInertiaLogRate);
                zoomInertiaLogRate_ =
                    zoomInertiaLogRate_ * (1.0 - kVelocitySmoothing) +
                    instRate * kVelocitySmoothing;
            }
            zoomInertiaAnchor_ = pointOnEarth;
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

    lastPinchTimestamp_ = timestamp;

    logGestureDiag(isPinchStartFrame ? "pinchStart" : "pinchMove",
                   centerX, centerY);
}

void CameraController::onPinchEnd() {
    logGestureDiag("pinchEnd", pinchAnchorScreenX_, pinchAnchorScreenY_);
    pinching_ = false;
    inertiaAngularVelocity_ = 0.0;
    // 松手时若刚才在缩放且留有足够动量，启动 zoom 惯性滑行（锚点仍需保留以
    // 沿视线朝它 dolly）。否则清零。
    if (hasPinchAnchor_ &&
        std::abs(zoomInertiaLogRate_) >= kMinZoomInertiaLogRate) {
        hasZoomInertia_ = true;
    } else {
        hasZoomInertia_ = false;
        zoomInertiaLogRate_ = 0.0;
    }
    hasPinchAnchor_ = false;
}

void CameraController::update(double deltaSeconds) {
    // 测量台冻结：完全空转，让相机停在最近一次显式位姿上，逐帧字节稳定。
    // 惯性/zoom 惯性/orbit 重建全部跳过 → far 位姿在重载耦合态下也可复现。
    if (measurementFreeze_) {
        return;
    }
    // 脚本化平移(测量台,§14.1② live 换页净测):每帧原地偏航一步,绕相机所在
    // 局部垂直轴(= 相机位置的径向,eye 在该轴上故不动、仅方位角扫掠),持续把新
    // 影像子瓦片带进视野 → 逼 live page-in。内部帧计数确定性,frames 帧后 hold
    // (停止扫掠让相机 settle,可截图看 ghost 是否随 settle 消退)。跳过惯性/orbit。
    if (scriptedPanActive_) {
        const int f = scriptedPanFrame_++;  // 内部时钟始终推进(确定性帧计数)
        // [0, start) 先 hold 让冷启动场景 settle 到 crisp 初始位姿;
        // [start, start+frames) 每帧原地偏航一步扫掠;之后 hold 让相机 settle。
        if (f >= scriptedPanStartFrame_ &&
            f < scriptedPanStartFrame_ + scriptedPanFrames_) {
            applyRotationAroundAxis(camera_->position().raw(),
                                    scriptedPanYawPerFrameRad_);
        }
        return;
    }
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

    // Zoom 惯性滑行：沿视线朝锚点按对数距离指数逼近（distance*=exp(-r·dt)），
    // 永不越过锚点、天然有界；沿 eye→anchor 直线 dolly 故锚点保持钉住。
    if (!dragging_ && !pinching_ && hasZoomInertia_ && deltaSeconds > 0.0) {
        const glm::dvec3 eye = camera_->position().raw();
        const glm::dvec3 toEye = eye - zoomInertiaAnchor_;
        const double dist = glm::length(toEye);
        if (dist > 1e-3) {
            const double sFrame =
                std::exp(zoomInertiaLogRate_ * deltaSeconds);  // >1 拉近
            glm::dvec3 nextEye = zoomInertiaAnchor_ + toEye / sFrame;
            nextEye = clampEyeAltitude(nextEye);
            if ((glm::length(nextEye) / kEarthRadiusMeters) <=
                kMaxDistanceEarthRadii) {
                camera_->setView(Vec3(nextEye), camera_->direction(),
                                 camera_->up());
                syncDistanceFromCamera();
            }
        }
        zoomInertiaLogRate_ *=
            std::exp(-kZoomInertiaDampingPerSecond * deltaSeconds);
        if (std::abs(zoomInertiaLogRate_) < kMinZoomInertiaLogRate) {
            hasZoomInertia_ = false;
            zoomInertiaLogRate_ = 0.0;
        }
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

void CameraController::setMeasurementFreeze(bool frozen) {
    measurementFreeze_ = frozen;
    if (frozen) {
        // 冻结瞬间清零所有惯性，避免残留速度在解冻前被"锁"进状态。
        inertiaAngularVelocity_ = 0.0;
        hasZoomInertia_ = false;
        zoomInertiaLogRate_ = 0.0;
    }
}

void CameraController::setScriptedPan(bool active, int startFrame, int frames,
                                     double yawPerFrameRad) {
    scriptedPanActive_ = active;
    scriptedPanStartFrame_ = startFrame;
    scriptedPanFrames_ = frames;
    scriptedPanYawPerFrameRad_ = yawPerFrameRad;
    scriptedPanFrame_ = 0;
    if (active) {
        // 启动瞬间清零惯性,避免残留速度叠加进脚本轨迹(破坏确定性)。
        inertiaAngularVelocity_ = 0.0;
        hasZoomInertia_ = false;
        zoomInertiaLogRate_ = 0.0;
    }
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
    return clampEyeToMinAltitude(
        eye, terrainHeightFunc_, lastKnownTerrainHeight_);
}

namespace {

// 相机 direction 在 pos 处本地 ENU 平面上的方位角：0=正北，顺时针(向东)为正。
double headingFromFrame(const glm::dvec3& localUp,
                        const glm::dvec3& direction,
                        const glm::dvec3& cameraUp) {
    glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), localUp);
    const double eastLen = glm::length(east);
    east = eastLen > 1e-9 ? east / eastLen : glm::dvec3(1.0, 0.0, 0.0);  // 极点退化
    const glm::dvec3 north = glm::cross(localUp, east);

    // 视线水平分量；近正俯视时退化，用相机 up 的水平分量兜底（屏幕"上"朝向）。
    glm::dvec3 horiz = direction - localUp * glm::dot(direction, localUp);
    double hLen = glm::length(horiz);
    if (hLen < 1e-9) {
        horiz = cameraUp - localUp * glm::dot(cameraUp, localUp);
        hLen = glm::length(horiz);
        if (hLen < 1e-9) return 0.0;
    }
    horiz /= hLen;
    double heading = std::atan2(glm::dot(horiz, east), glm::dot(horiz, north));
    if (heading < 0.0) heading += 2.0 * glm::pi<double>();
    return heading;
}

}  // namespace

double CameraController::headingRadians() const {
    const glm::dvec3 up =
        Ellipsoid::WGS84().geodeticSurfaceNormal(camera_->position()).raw();
    return headingFromFrame(up, camera_->direction().raw(), camera_->up().raw());
}

double CameraController::pitchRadians() const {
    const glm::dvec3 up =
        Ellipsoid::WGS84().geodeticSurfaceNormal(camera_->position()).raw();
    const double s = std::clamp(glm::dot(camera_->direction().raw(), up),
                                -1.0, 1.0);
    return std::asin(s);  // + 向上，- 向下；正俯视 = -π/2
}

void CameraController::resetNorthUp() {
    update(0.0);
    orbitMode_ = false;
    inertiaAngularVelocity_ = 0.0;
    hasZoomInertia_ = false;
    zoomInertiaLogRate_ = 0.0;

    double heading = headingRadians();
    if (heading > glm::pi<double>()) heading -= 2.0 * glm::pi<double>();  // 走最短
    if (std::abs(heading) < 1e-6) return;

    // 绕相机自身竖轴原地旋转：相机位置在该轴上不动 → 俯仰精确保持，仅朝向转到
    // 正北（与 cesium camera.setView({heading:0}) 同）。绕竖轴旋转 α 使 heading
    // 变化 -α，故抵消 heading 需转 +heading。
    const glm::dvec3 axis =
        Ellipsoid::WGS84().geodeticSurfaceNormal(camera_->position()).raw();
    rotateCameraAroundPoint(camera_->position().raw(), axis, heading);
}

bool CameraController::debugAnchorWorld(Vec3& outWorld) const {
    if (pinching_ && hasPinchAnchor_) {
        outWorld = Vec3(pinchAnchorNormal_.raw() * grabbedRadiusMeters_);
        return true;
    }
    if (dragging_ && hasGrabbedPoint_) {
        outWorld = grabbedPoint_;
        return true;
    }
    return false;
}

void CameraController::logGestureDiag(const char* label, float screenX, float screenY) {
    // [GESTDIAG] 临时插桩：定位"双指触摸瞬间偏移"。dEye 是本事件相机位移，
    // anchorErr 是锚点当前投影与手指的像素差；某帧 dEye 或 anchorErr 突然
    // 变大，即为跳变帧。定位后整体移除。
    const glm::dvec3 eye = camera_->position().raw();
    const double dEye = hasLastDiagEye_ ? glm::length(eye - lastDiagEye_) : 0.0;
    lastDiagEye_ = eye;
    hasLastDiagEye_ = true;

    const Cartographic cart =
        Ellipsoid::WGS84().cartesianToCartographic(Vec3(eye));

    bool hasAnchor = false;
    glm::dvec3 anchorWorld(0.0);
    if (pinching_ && hasPinchAnchor_) {
        anchorWorld = pinchAnchorNormal_.raw() * grabbedRadiusMeters_;
        hasAnchor = true;
    } else if (hasGrabbedPoint_) {
        anchorWorld = grabbedPoint_.raw();
        hasAnchor = true;
    }

    double anchorErrX = 0.0;
    double anchorErrY = 0.0;
    if (hasAnchor) {
        const glm::dmat4 vp = camera_->viewProjectionMatrix(
            static_cast<double>(viewportWidth_),
            static_cast<double>(viewportHeight_)).raw();
        glm::dvec4 clip = vp * glm::dvec4(anchorWorld, 1.0);
        if (std::abs(clip.w) > 1e-9) {
            clip /= clip.w;
            const double sx = (clip.x + 1.0) * 0.5 * viewportWidth_;
            const double sy = (1.0 - clip.y) * 0.5 * viewportHeight_;
            anchorErrX = sx - static_cast<double>(screenX);
            anchorErrY = sy - static_cast<double>(screenY);
        }
    }

    platformLog(LogLevel::Info, "GESTDIAG",
        "%s eye=(%.5f,%.5f,%.0fm) dEye=%.1fm finger=(%.0f,%.0f) "
        "anchorErr=(%.1f,%.1f)px hasAnchor=%d",
        label,
        cart.longitudeDegrees(), cart.latitudeDegrees(), cart.height(),
        dEye, screenX, screenY, anchorErrX, anchorErrY, hasAnchor ? 1 : 0);
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

    // move 期锚定在抓取球面（半径=抓取点半径），不重 pick 地形：from/to 同
    // 球面才能一次旋转把锚点精确放回指下。重 pick 地形时，指下地形高≠抓取
    // 点高，法线对齐后锚点投影偏离手指（起伏越大/视角越斜越明显）＝不跟手。
    Vec3 targetPoint;
    bool anchorValid = false;
    if (hasGrabbedPoint_) {
        const Ray ray = camera_->getPickRay(
            static_cast<double>(xPixels),
            static_cast<double>(yPixels),
            static_cast<double>(viewportWidth_),
            static_cast<double>(viewportHeight_));
        anchorValid = intersectGrabSphere(ray, targetPoint);
    }

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
