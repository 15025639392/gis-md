#include "CameraSystem.h"
#include "CameraPoseOps.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../scene/Camera.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace earth_engine {

namespace {

constexpr double kEarthRadiusMeters = 6378137.0;
// 相机包络常量（净空下限 / 地心距上限）归 CameraConstraintSolver 单点持有；
// viewDistance 的钳位与手势侧的 dolly 封顶必须读同一份。
constexpr double kMinAltitudeMeters = CameraSystem::kMinClearanceMeters;
constexpr double kMaxDistanceEarthRadii =
    CameraConstraintSolver::kMaxDistanceEarthRadii;

} // namespace

CameraSystem::CameraSystem(Camera* camera)
    : camera_(camera) {
    // 注册控制器族。首个注册者自动成为活动控制器,不留半初始化窗口。
    auto freeGlobe =
        std::make_unique<FreeGlobeController>(camera, &constraintSolver_);
    freeGlobe_ = freeGlobe.get();
    selector_.add(kFreeGlobeController, std::move(freeGlobe));

    // Default to Chongqing area for testing
    const auto& e = Ellipsoid::WGS84();
    auto target = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.508, 29.617, 0.0));
    auto eye = e.cartographicToCartesian(
        Cartographic::fromDegrees(106.508, 29.617, 1500.0));
    camera_->lookAt(eye, target, e.geodeticSurfaceNormal(target));
}

void CameraSystem::setViewport(int widthPixels, int heightPixels) {
    // 广播:未激活的控制器也要保持正确,否则接管瞬间用错像素→角度增益。
    selector_.setViewport(widthPixels, heightPixels);
}

void CameraSystem::setSurfacePicker(SurfacePicker picker) {
    freeGlobe_->setSurfacePicker(std::move(picker));
}

void CameraSystem::setTerrainHeightFunc(TerrainHeightFunc func) {
    constraintSolver_.setTerrainHeightFunc(std::move(func));
}

void CameraSystem::setTerrainAreaSampleFunc(TerrainAreaSampleFunc func) {
    constraintSolver_.setTerrainAreaSampleFunc(std::move(func));
}

void CameraSystem::setTerrainRevisionFunc(std::function<uint64_t()> func) {
    constraintSolver_.setTerrainRevisionFunc(std::move(func));
}

// ============================================================
// 手势转发
// ============================================================

void CameraSystem::syncFrameBeforeGesture() {
    update(0.0);
}

void CameraSystem::onDragStart(float xPixels, float yPixels,
                                   double timestamp) {
    auto* c = selector_.activeAs<FreeGlobeController>();
    if (!c) return;  // 非 Free 控制器在驱动(如飞行中):触摸事件丢弃
    syncFrameBeforeGesture();
    c->onDragStart(xPixels, yPixels, timestamp);
}

void CameraSystem::onDragMove(float xPixels, float yPixels,
                                  double timestamp) {
    if (auto* c = selector_.activeAs<FreeGlobeController>()) {
        c->onDragMove(xPixels, yPixels, timestamp);
    }
}

void CameraSystem::onDragEnd() {
    if (auto* c = selector_.activeAs<FreeGlobeController>()) {
        c->onDragEnd();
    }
}

void CameraSystem::onPinchGesture(const PinchInput& input) {
    auto* c = selector_.activeAs<FreeGlobeController>();
    if (!c) return;
    if (!c->pinching()) {
        syncFrameBeforeGesture();
    }
    c->onPinchGesture(input);
}

void CameraSystem::onPinchGesture(float scale,
                                      float centerX,
                                      float centerY,
                                      float rotationRadians,
                                      float centerDeltaX,
                                      float centerDeltaY,
                                      double timestamp) {
    // 非法 spread 在操控器侧同样早退；这里先判一次是为了不给它跑同步帧
    // （旧实现里那一帧发生在适配器的早退之后）。
    if (scale <= 0.0f) return;
    auto* c = selector_.activeAs<FreeGlobeController>();
    if (!c) return;
    if (!c->pinching()) {
        syncFrameBeforeGesture();
    }
    c->onPinchGesture(scale, centerX, centerY, rotationRadians,
                      centerDeltaX, centerDeltaY, timestamp);
}

void CameraSystem::onPinchEnd() {
    if (auto* c = selector_.activeAs<FreeGlobeController>()) {
        c->onPinchEnd();
    }
}

bool CameraSystem::debugAnchorWorld(Vec3& outWorld) const {
    return freeGlobe_->debugAnchorWorld(outWorld);
}

bool CameraSystem::selectController(const std::string& name) {
    return selector_.select(name);
}

const std::string& CameraSystem::activeControllerName() const {
    return selector_.activeName();
}

// ============================================================
// 帧循环
// ============================================================

void CameraSystem::update(double deltaSeconds) {
    constraintSolver_.beginFrame();  // 探针"每帧至多重建一次"的帧时钟
    updateInternal(deltaSeconds);
    // 帧末哨兵：兜底收编所有未经操控器 clampNow 路由的位姿写入（viewDistance /
    // setNadirOrbitView / scriptedPan / Facade/JNI 绕过控制器的裸写）。
    resolveAtFrameEnd(deltaSeconds);
}

void CameraSystem::updateInternal(double deltaSeconds) {
    // 测量台冻结：完全空转，让相机停在最近一次显式位姿上，逐帧字节稳定。
    // 惯性/zoom 惯性全部跳过 → far 位姿在重载耦合态下也可复现。
    if (measurementFreeze_) {
        return;
    }
    // 脚本化平移(测量台,§14.1② live 换页净测):每帧原地偏航一步,绕相机所在
    // 局部垂直轴(= 相机位置的径向,eye 在该轴上故不动、仅方位角扫掠),持续把新
    // 影像子瓦片带进视野 → 逼 live page-in。内部帧计数确定性,frames 帧后 hold
    // (停止扫掠让相机 settle,可截图看 ghost 是否随 settle 消退)。跳过惯性。
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

    if (ICameraController* active = selector_.active()) {
        active->tick(deltaSeconds);
    }
}

bool CameraSystem::resolveAtFrameEnd(double deltaSeconds) {
    // 冻结契约要求位姿逐帧字节稳定，哨兵绝不触碰。
    if (measurementFreeze_) {
        return false;
    }
    // 位姿指纹比对：帧末发现位姿与上次解算结果不同 ⇒ 期间有未经 clampNow 的
    // 写入（viewDistance/setNadirOrbitView/回中/scriptedPan/外部裸写）——都是
    // 用户或调用方主动为之，按 user-driven 处理（滤波立即）。
    // 数据驱动 = 位姿没动、只有地形样本变。
    const bool userDriven = constraintSolver_.poseChangedSince(
        camera_->position().raw(), camera_->direction().raw());

    const glm::dvec3 eye = camera_->position().raw();
    const glm::dvec3 clamped = constraintSolver_.constrainEye(
        eye, userDriven, deltaSeconds, nullptr);
    const bool changed = glm::length(clamped - eye) > 1e-6;
    if (changed) {
        camera_->setView(Vec3(clamped), camera_->direction(), camera_->up());
    }
    commitResolvedPose();
    return changed;
}

void CameraSystem::commitResolvedPose() {
    constraintSolver_.commitPose(camera_->position().raw(),
                                 camera_->direction().raw());
}

// ============================================================
// 测量台
// ============================================================

void CameraSystem::setMeasurementFreeze(bool frozen) {
    measurementFreeze_ = frozen;
    if (frozen) {
        // 冻结瞬间清零所有惯性，避免残留速度在解冻前被"锁"进状态。
        freeGlobe_->clearAllInertia();
    }
}

void CameraSystem::setScriptedPan(bool active, int startFrame, int frames,
                                     double yawPerFrameRad) {
    scriptedPanActive_ = active;
    scriptedPanStartFrame_ = startFrame;
    scriptedPanFrames_ = frames;
    scriptedPanYawPerFrameRad_ = yawPerFrameRad;
    scriptedPanFrame_ = 0;
    if (active) {
        // 启动瞬间清零惯性,避免残留速度叠加进脚本轨迹(破坏确定性)。
        freeGlobe_->clearAllInertia();
    }
}

// ============================================================
// 位姿设定与只读派生量
// ============================================================

float CameraSystem::distance() const {
    return static_cast<float>(camera_->position().length() / kEarthRadiusMeters);
}

glm::dquat CameraSystem::rotation() const {
    // 旧 orbit 真值 rotation_ 满足 rotation_·(+Z)=direction、rotation_·(+Y)=up，
    // 故列向量 [Rx,Ry,Rz] = [cross(up,dir), up, dir]（cross(up,dir) = -right，
    // 这样才右手正交：identity 位姿 dir=+Z/up=+Y 下恰得单位阵）。
    const glm::dvec3 f = camera_->direction().raw();
    const glm::dvec3 u = camera_->up().raw();
    return glm::quat_cast(glm::dmat3(glm::cross(u, f), u, f));
}

void CameraSystem::setNadirOrbitView(const Vec3& targetEcef,
                                         const Vec3& surfaceUpNormal,
                                         double heightMeters) {
    // 目标点局部 ENU:east = z × up(极点退化时回退 ECEF X 轴),north = up × east。
    const glm::dvec3 upG = glm::normalize(surfaceUpNormal.raw());
    glm::dvec3 eastG = glm::cross(glm::dvec3(0.0, 0.0, 1.0), upG);
    if (glm::length(eastG) < 1e-9) {
        eastG = glm::dvec3(1.0, 0.0, 0.0);
    }
    eastG = glm::normalize(eastG);
    const glm::dvec3 northG = glm::normalize(glm::cross(upG, eastG));
    // 旧 orbit 约定 eye = -(rotation_·+Z)·distance_·R / up = rotation_·+Y,
    // 其中 rotation_ 的列 = [-east, north, -up] ⇒ 展开即下式。视线指向**地心**
    // (不是 targetEcef)——原样保留旧语义,大地法线不过地心故 eye 并不严格在
    // target 正上方,差异 ~11'。
    const double targetRadius = std::sqrt(targetEcef.dot(targetEcef));
    const glm::dvec3 eye = upG * (targetRadius + heightMeters);
    camera_->lookAt(Vec3(eye), Vec3::zero(), Vec3(northG));
    freeGlobe_->clearPanInertia();
}

void CameraSystem::viewDistance(const Vec3& targetWorld, double distanceMeters) {
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
    freeGlobe_->clearPanInertia();
}

void CameraSystem::applyRotationAroundAxis(const glm::dvec3& axis, double angle) {
    if (glm::length(axis) < 1e-10 || std::abs(angle) < 1e-12) {
        return;
    }
    camera_ops::rotateAboutOrigin(
        *camera_, glm::angleAxis(angle, glm::normalize(axis)));
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

double CameraSystem::headingRadians() const {
    const glm::dvec3 up =
        Ellipsoid::WGS84().geodeticSurfaceNormal(camera_->position()).raw();
    return headingFromFrame(up, camera_->direction().raw(), camera_->up().raw());
}

double CameraSystem::pitchRadians() const {
    const glm::dvec3 up =
        Ellipsoid::WGS84().geodeticSurfaceNormal(camera_->position()).raw();
    const double s = std::clamp(glm::dot(camera_->direction().raw(), up),
                                -1.0, 1.0);
    return std::asin(s);  // + 向上，- 向下；正俯视 = -π/2
}

void CameraSystem::resetNorthUp() {
    update(0.0);
    freeGlobe_->clearGlideInertia();

    double heading = headingRadians();
    if (heading > glm::pi<double>()) heading -= 2.0 * glm::pi<double>();  // 走最短
    if (std::abs(heading) < 1e-6) return;

    // 绕相机自身竖轴原地旋转：相机位置在该轴上不动 → 俯仰精确保持，仅朝向转到
    // 正北（与 cesium camera.setView({heading:0}) 同）。绕竖轴旋转 α 使 heading
    // 变化 -α，故抵消 heading 需转 +heading。
    const glm::dvec3 axis =
        Ellipsoid::WGS84().geodeticSurfaceNormal(camera_->position()).raw();
    camera_ops::rotateAboutPoint(*camera_, camera_->position().raw(),
                                 axis, heading);
}

} // namespace earth_engine
