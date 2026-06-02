#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace earth_engine {

class Camera;

/// Arcball 轨道相机控制器。
/// 将 2D 触控输入转换为 3D 相机 orbit rotation。
/// 支持 drag 旋转、pinch 缩放和惯性阻尼。
class CameraController {
public:
    /// @param camera 受控相机（非空，生命周期由调用者管理）
    explicit CameraController(Camera* camera);

    /// 设置视口尺寸（用于 arcball 投影计算）
    void setViewport(int widthPixels, int heightPixels);

    /// drag 开始（手指按下）
    /// @param timestamp 单调时钟时间戳（秒），用于惯性角速度计算
    void onDragStart(float xPixels, float yPixels, double timestamp = 0.0);

    /// drag 移动（手指滑动）
    void onDragMove(float xPixels, float yPixels, double timestamp = 0.0);

    /// drag 结束（手指抬起，启动惯性）
    void onDragEnd();

    /// pinch 缩放（累积缩放因子，1.0 = 初始状态）
    void onPinch(float scale);

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
    /// 计算地球在屏幕上的投影半径（像素）
    float projectedGlobeRadiusPixels() const;

    /// 屏幕坐标 → arcball 单位球面上的点
    glm::vec3 mapToArcball(float xPixels, float yPixels) const;

    /// 应用 orbit 旋转
    /// @param timestamp 本次事件的时间戳（秒），用于角速度计算
    void orbit(float startX, float startY, float endX, float endY,
               double timestamp);

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

    // 惯性状态
    glm::dvec3 inertiaAxis_{0.0, 1.0, 0.0};
    double inertiaAngularVelocity_ = 0.0;
    double lastDragTimestamp_ = 0.0;  // 最近一次 drag 事件的时间戳

    // pinch 状态
    float pinchBaseDistance_ = 7.0f;
    bool pinching_ = false;
};

} // namespace earth_engine
