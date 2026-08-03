#pragma once

#include "../core/math/Vec3.h"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <functional>
#include <optional>

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

    /// Returns height above WGS84 ellipsoid (meters) at the given ECEF position,
    /// or nullopt when no terrain data covers the point (unloaded / no coverage).
    /// When set, collision clamping uses terrain height instead of bare ellipsoid;
    /// on nullopt the clamp holds the last known terrain height rather than
    /// treating the point as sea level (which would let the eye sink into
    /// not-yet-loaded terrain).
    using TerrainHeightFunc =
        std::function<std::optional<double>(const Vec3& ecefPosition)>;
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

    /// 北极星测量台冻结开关：置 true 后 update() 变成完全空操作——不跑惯性、
    /// 不跑 zoom 惯性、不做 orbit 重建、不碰相机。相机停在最近一次显式
    /// lookAt/viewDistance 设定的位姿上，逐帧字节稳定，让重载耦合态（高空 + 深
    /// 影像 churn）下的 far 位姿也精确可复现（对拍去耦前后必须同位姿）。
    /// 启用时顺带清零所有惯性状态，避免冻结瞬间残留速度被"锁"进去。
    void setMeasurementFreeze(bool frozen);
    bool measurementFrozen() const { return measurementFreeze_; }

    /// 北极星测量台脚本化确定性平移(净测 §14.1② live 换页 ghost):置 active 后
    /// update() 每帧对当前视线施加固定 yawPerFrameRad 的"原地偏航"(绕相机所在
    /// 局部垂直轴,eye 不动、方位角扫掠 → 持续 page-in),共 frames 帧后 hold。
    /// 帧计数为内部计数器(每次 update 递增)故轨迹确定性、与 wall-clock/掉帧无关;
    /// 无惯性介入。与 measurementFreeze 互斥(freeze 优先)。free swipe 惯性漂不可控,
    /// 此脚本给可复现的受控运动,配 PageDet/截图量 ghost/stall。
    void setScriptedPan(bool active, int startFrame, int frames,
                        double yawPerFrameRad);
    bool scriptedPanActive() const { return scriptedPanActive_; }

    // ---- 相机状态 ----

    /// 设置相机到地球中心的距离（地球半径单位，默认 7.0）
    void setDistance(float earthRadii);
    float distance() const { return distance_; }

    /// 获取当前旋转四元数
    const glm::dquat& rotation() const { return rotation_; }

    /// 直接设置旋转
    void setRotation(const glm::dquat& q);

    /// 把 orbit 状态设为"目标点正上方 heightMeters、正北朝上、看向地心
    /// (nadir)"。orbit 约定(eye = -(rotation_·+Z)·distance_·R,
    /// up = rotation_·+Y)是本类的私有实现细节:外部调用方(如
    /// EarthEngineSdkFacade::resetCamera)一律走本接口,不得在类外复刻
    /// 四元数推导——约定变更只需要改本类。
    /// @param targetEcef      目标地表点(ECEF)
    /// @param surfaceUpNormal 目标点的大地法线(调用方从椭球取,本类不依赖
    ///                        Ellipsoid;传入无需归一化)
    /// @param heightMeters    相机在目标点上方的高度(米)
    void setNadirOrbitView(const Vec3& targetEcef,
                           const Vec3& surfaceUpNormal,
                           double heightMeters);

    /// 保持当前 target→eye 方位，把相机放到 target 外指定距离并看向 target。
    void viewDistance(const Vec3& targetWorld, double distanceMeters);

    /// [GESTDIAG] 当前手势锚点世界坐标(ECEF)。有活动 drag/pinch 锚点时返回
    /// true 并写出 outWorld；否则返回 false。用于真机可视化锚点稳定性。
    bool debugAnchorWorld(Vec3& outWorld) const;

    /// 相机方位角（弧度，0 = 正北，顺时针为正）。用于指北针。
    double headingRadians() const;
    /// 相机俯仰角（弧度，0 = 水平，-π/2 = 正俯视）。
    double pitchRadians() const;
    /// 复位到正北朝上（heading = 0），保持屏幕中心地物居中与当前俯仰。
    void resetNorthUp();

private:
    /// 锚点钉合求解：求把"像素 (x,y) 射线与抓取球的交点方向"转到 anchorNormal
    /// 的绕地心旋转。单指拖拽与双指钉合共用同一份数学（同构，见 applyAnchorDrag
    /// 与 keepAnchorAtScreenPoint），任何一侧的修正必须同时作用于另一侧。
    struct AnchorSolveResult {
        glm::dquat delta{1.0, 0.0, 0.0, 0.0};
        /// 入射余弦 |dot(rayDir, 求解点法线)|：1=正对，→0=掠射；最近接近点
        /// 恒为 0（射线在该点与球面相切向正交）。病态区权重的输入。
        double conditioning = 0.0;
        bool valid = false;       ///< 求出了球面点（真命中或最近接近点）
        bool hit = false;         ///< 射线真正命中抓取球
        bool degenerate = false;  ///< from≈to，无需旋转
    };
    AnchorSolveResult solveAnchorRotation(const Vec3& anchorNormal,
                                          float xPixels,
                                          float yPixels) const;
    /// 抓取球上"该射线之下"的点：真交点，或 miss 时的最近接近点（相切处
    /// 与真交点重合 → 跨球缘连续）。球心在射线后方等极端退化时返回 false。
    bool pointOnGrabSphere(const Ray& ray, Vec3& outPoint, bool& outTrueHit) const;
    /// 转台式旋转增量：屏幕像素位移按 fov/height 转角度（相对 dragLast）。
    glm::dquat spinTurntableDelta(float xPixels, float yPixels) const;
    /// 按当前模式施加旋转增量（orbit 模式转 rotation_，自由模式转相机整体）。
    void applyRotationDelta(const glm::dquat& delta);

    bool intersectGrabSphere(const Ray& ray, Vec3& outPoint) const;
    static bool intersectSphere(const Ray& ray, double radiusMeters,
                                Vec3& outPoint);
    /// 把拾取点重投影到该像素的射线上（半径不变，只换方向）。掠射角下同半径
    /// 球可能被错过，此时返回 false 且不改动 point。
    bool snapPickOntoRay(float xPixels, float yPixels, Vec3& point) const;
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
    /// 高空 zoom-out 回中：按本步拉远的对数距离步长，把视线向地心方向收敛，
    /// 让球心随缩放进度逐步回到屏幕中心（低空不介入，见 .cpp 常量说明）。
    void applyHighAltitudeRecenter(double zoomOutLogStep);
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
    // 最近一次有效地形高度样本(米),供地形无数据时的 clamp 保守回退。
    // mutable:clampEyeAltitude 是 const 查询但需更新此缓存。
    mutable double lastKnownTerrainHeight_ = 0.0;
    int viewportWidth_ = 1;
    int viewportHeight_ = 1;

    glm::dquat rotation_{1.0, 0.0, 0.0, 0.0};
    float distance_ = 7.0f;
    bool orbitMode_ = true;
    // 测量台冻结：true 时 update() 完全空转（见 setMeasurementFreeze）。
    bool measurementFreeze_ = false;

    // 测量台脚本化平移(见 setScriptedPan):active 时 update() 每帧原地偏航一步,
    // 内部帧计数确定性驱动,frames 帧后 hold。
    bool scriptedPanActive_ = false;
    int scriptedPanStartFrame_ = 0;  // 扫掠前先 hold 的帧数(让冷启动 settle)
    int scriptedPanFrames_ = 0;
    int scriptedPanFrame_ = 0;
    double scriptedPanYawPerFrameRad_ = 0.0;

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
    double lastPinchTimestamp_ = 0.0;
    // 单事件 jerk 限幅削掉的缩放量（对数空间），由后续事件在限幅余量内补回，
    // 使一段捏合的总缩放倍数与手指分开倍数一致。手势起止清零。
    double pinchScaleResidualLog_ = 0.0;

    // zoom 惯性状态（对数距离空间，见 .cpp 常量说明）
    bool hasZoomInertia_ = false;
    double zoomInertiaLogRate_ = 0.0;         // d(ln dist)/dt，>0 = 继续拉近
    glm::dvec3 zoomInertiaAnchor_{0.0, 0.0, 0.0};

    // [GESTDIAG] 临时：上一次手势事件时的相机 eye（算 dEye 用）。
    glm::dvec3 lastDiagEye_{0.0, 0.0, 0.0};
    bool hasLastDiagEye_ = false;
};

} // namespace earth_engine
