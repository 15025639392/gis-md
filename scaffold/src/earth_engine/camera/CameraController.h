#pragma once

#include "../core/math/Vec3.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <functional>

namespace earth_engine {

class Camera;
class Ray;

/// Anchor-based globe camera controller.
/// 单指拖拽先抓取地表点，移动时让该点尽量跟随手指。
/// 双指手势围绕双指中心下方的地表锚点缩放、旋转和倾斜。
class CameraController {
public:
    /// @param camera 受控相机（非空，生命周期由调用者管理）
    explicit CameraController(Camera* camera);

    /// 设置视口尺寸（用于 pick ray 和屏幕坐标归一化）
    void setViewport(int widthPixels, int heightPixels);

    /// Scene 可注入地形拾取链路；未注入或未命中时回退到控制器
    /// 内部的 WGS84 球面拾取。
    using SurfacePicker = std::function<bool(float xPixels, float yPixels, Vec3& outPoint)>;
    void setSurfacePicker(SurfacePicker picker);

    /// Returns height above WGS84 ellipsoid (meters) at the given ECEF position.
    /// When set, collision clamping uses terrain height instead of bare ellipsoid.
    using TerrainHeightFunc = std::function<double(const Vec3& ecefPosition)>;
    void setTerrainHeightFunc(TerrainHeightFunc func);

    /// drag 开始（手指按下）
    /// @param timestamp 单调时钟时间戳（秒），用于惯性角速度计算
    void onDragStart(float xPixels, float yPixels, double timestamp = 0.0);

    /// drag 移动（手指滑动）
    void onDragMove(float xPixels, float yPixels, double timestamp = 0.0);

    /// drag 结束（手指抬起，启动惯性）
    void onDragEnd();

    void onPinchGesture(float scale,
                        float centerX,
                        float centerY,
                        float rotationRadians,
                        float centerDeltaX,
                        float centerDeltaY,
                        double timestamp = 0.0);
    void onPinchEnd();

    /// 时间步进（更新惯性动画）
    /// @param deltaSeconds 上一帧到现在的秒数
    void update(double deltaSeconds);

    // ---- 相机状态 ----

    /// 设置相机到地球中心的距离（地球半径单位，默认 7.0）
    void setDistance(float earthRadii);
    float distance() const { return distance_; }

    /// 获取当前旋转四元数
    const glm::dquat& rotation() const { return rotation_; }

    /// 直接设置旋转
    void setRotation(const glm::dquat& q);

    /// 保持当前 target→eye 方位，把相机放到 target 外指定距离并看向 target。
    void viewDistance(const Vec3& targetWorld, double distanceMeters);

    /// [GESTDIAG] 当前手势锚点世界坐标(ECEF)。有活动 drag/pinch 锚点时返回
    /// true 并写出 outWorld；否则返回 false。用于真机可视化锚点稳定性。
    bool debugAnchorWorld(Vec3& outWorld) const;

private:
    bool intersectGrabSphere(const Ray& ray, Vec3& outPoint) const;
    bool pickSurfacePoint(float xPixels, float yPixels, Vec3& outPoint) const;
    bool grabSurfacePoint(float xPixels, float yPixels);
    void applyAnchorDrag(float xPixels, float yPixels, double timestamp);
    void applyRotationAroundAxis(const glm::dvec3& axis, double angle);
    void keepAnchorAtScreenPoint(const Vec3& anchorNormal, float xPixels, float yPixels);
    void rotateCameraAroundPoint(const glm::dvec3& center,
                                 const glm::dvec3& axis,
                                 double angle);
    void rotateCameraVerticalAroundPoint(const glm::dvec3& center,
                                         double angle,
                                         double minSlope);
    void applyCameraRotation(const glm::dquat& delta);
    void syncDistanceFromCamera();

    /// Clamps eye to at least the configured visual floor above terrain/ellipsoid.
    glm::dvec3 clampEyeAltitude(const glm::dvec3& eye) const;

    /// [GESTDIAG] 临时插桩：每个手势事件后打印相机位移量与锚点投影误差，
    /// 用于真机定位"双指触摸瞬间偏移"。定位后连同 lastDiagEye_ 一并移除。
    void logGestureDiag(const char* label, float screenX, float screenY);

    Camera* camera_;
    SurfacePicker surfacePicker_;
    TerrainHeightFunc terrainHeightFunc_;
    int viewportWidth_ = 1;
    int viewportHeight_ = 1;

    glm::dquat rotation_{1.0, 0.0, 0.0, 0.0};
    float distance_ = 7.0f;
    bool orbitMode_ = true;

    // drag 状态
    bool dragging_ = false;
    float dragStartX_ = 0.0f;
    float dragStartY_ = 0.0f;
    float dragLastX_ = 0.0f;
    float dragLastY_ = 0.0f;
    bool hasGrabbedPoint_ = false;
    Vec3 grabbedNormal_{0.0, 0.0, 1.0};
    Vec3 grabbedPoint_{0.0, 0.0, 6378137.0};
    double grabbedRadiusMeters_ = 6378137.0;

    // 惯性状态
    glm::dvec3 inertiaAxis_{0.0, 1.0, 0.0};
    double inertiaAngularVelocity_ = 0.0;
    double lastDragTimestamp_ = 0.0;  // 最近一次 drag 事件的时间戳

    // pinch 状态
    bool pinching_ = false;
    bool hasPinchAnchor_ = false;
    Vec3 pinchAnchorNormal_{0.0, 0.0, 1.0};
    Vec3 pinchEarthUpNormal_{0.0, 0.0, 1.0};
    float pinchAnchorScreenX_ = 0.0f;
    float pinchAnchorScreenY_ = 0.0f;

    // [GESTDIAG] 临时：上一次手势事件时的相机 eye（算 dEye 用）。
    glm::dvec3 lastDiagEye_{0.0, 0.0, 0.0};
    bool hasLastDiagEye_ = false;
};

} // namespace earth_engine
