#pragma once

#include "../core/math/Vec3.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace earth_engine {

class Camera;
class Ray;

/// OpenGlobus-style anchor-based globe camera controller.
/// 单指拖拽先抓取 WGS84 椭球上的地表点，移动时让该点跟随手指。
/// 支持 drag 旋转、pinch 缩放和惯性阻尼。
class CameraController {
public:
    /// @param camera 受控相机（非空，生命周期由调用者管理）
    explicit CameraController(Camera* camera);

    /// 设置视口尺寸（用于 pick ray 和屏幕坐标归一化）
    void setViewport(int widthPixels, int heightPixels);

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

private:
    bool intersectGrabSphere(const Ray& ray, Vec3& outPoint) const;
    bool grabSurfacePoint(float xPixels, float yPixels);
    void applyAnchorDrag(float xPixels, float yPixels, double timestamp);
    void applyRotationAroundAxis(const glm::dvec3& axis, double angle);

    Camera* camera_;
    int viewportWidth_ = 1;
    int viewportHeight_ = 1;

    glm::dquat rotation_{1.0, 0.0, 0.0, 0.0};
    float distance_ = 7.0f;

    // drag 状态
    bool dragging_ = false;
    float dragStartX_ = 0.0f;
    float dragStartY_ = 0.0f;
    float dragLastX_ = 0.0f;
    float dragLastY_ = 0.0f;
    bool hasGrabbedPoint_ = false;
    Vec3 grabbedNormal_{0.0, 0.0, 1.0};
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
};

} // namespace earth_engine
