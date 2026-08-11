#include <gtest/gtest.h>

#include "earth_engine/camera/CameraConstraintSolver.h"
#include "earth_engine/camera/CameraPose.h"
#include "earth_engine/camera/CameraSystem.h"
#include "earth_engine/camera/Viewpoint.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFrameSeconds = 1.0 / 60.0;

Cartographic geo(double lonDeg, double latDeg, double h) {
    return Cartographic::fromDegrees(lonDeg, latDeg, h);
}

/// 解析式"山脊":以 ridgeLon 为中心的一条南北向山墙,高 peakMeters。
/// 用解析地形而不是真瓦片,是为了让"穿山路径全程 AGL ≥ 净空"这条判据在 host
/// 上确定性可判——真瓦片的加载时序会把判据变成概率性的。
struct AnalyticRidge {
    double ridgeLonDeg = 0.0;
    double halfWidthDeg = 2.0;
    double peakMeters = 6000.0;

    double heightAt(const Vec3& ecef) const {
        const Cartographic c =
            Ellipsoid::WGS84().cartesianToCartographic(ecef);
        const double d = std::abs(c.longitudeDegrees() - ridgeLonDeg);
        if (d >= halfWidthDeg) {
            return 0.0;
        }
        const double t = 1.0 - d / halfWidthDeg;
        return peakMeters * t * t;
    }
};

class FlightTest : public ::testing::Test {
protected:
    void SetUp() override {
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(45.0 * kPi / 180.0, 1.0, 5.0e7);
        system_ = std::make_unique<CameraSystem>(camera_.get());
        system_->setViewport(800, 600);
    }

    void installRidge(const AnalyticRidge& ridge) {
        ridge_ = ridge;
        const AnalyticRidge r = ridge;
        system_->setTerrainHeightFunc(
            [r](const Vec3& p) -> std::optional<double> {
                return r.heightAt(p);
            });
        system_->setTerrainAreaSampleFunc(
            [r](const Vec3& groundEcef, double,
                const std::vector<glm::dvec2>& offsets,
                std::vector<CameraSystem::TerrainSample>& out) {
                const auto& e = Ellipsoid::WGS84();
                const glm::dmat3 frame =
                    CameraPose::enuFrameAt(groundEcef.raw());
                out.clear();
                out.reserve(offsets.size());
                for (const glm::dvec2& off : offsets) {
                    const glm::dvec3 p = groundEcef.raw() +
                                         frame[0] * off.x + frame[1] * off.y;
                    CameraSystem::TerrainSample s;
                    s.valid = true;
                    s.heightMeters = r.heightAt(Vec3(p));
                    Cartographic c = e.cartesianToCartographic(Vec3(p));
                    c.setHeight(s.heightMeters);
                    s.surfaceEcef = e.cartographicToCartesian(c);
                    out.push_back(s);
                }
            });
    }

    void placeAt(const Cartographic& eye, double pitch) {
        Viewpoint vp;
        vp.eyeGeo = eye;
        vp.headingRadians = 0.0;
        vp.pitchRadians = pitch;
        vp.rollRadians = 0.0;
        system_->setViewpoint(vp);
        system_->update(kFrameSeconds);
    }

    /// 跑到飞行结束,返回帧数;每帧回调供逐帧判据。
    int flyToCompletion(const std::function<void()>& perFrame = {}) {
        int frames = 0;
        while (system_->cameraFlightActive() && frames < 2000) {
            system_->update(kFrameSeconds);
            ++frames;
            if (perFrame) perFrame();
        }
        return frames;
    }

    double currentAgl() const {
        const double h = Ellipsoid::WGS84()
                             .cartesianToCartographic(camera_->position())
                             .height();
        return h - ridge_.heightAt(camera_->position());
    }

    AnalyticRidge ridge_{0.0, 2.0, 0.0};
    std::unique_ptr<Camera> camera_;
    std::unique_ptr<CameraSystem> system_;
};

}  // namespace

// ============================================================
// 判据 ①:终点位姿相对误差 < 1e-3
// ============================================================

TEST_F(FlightTest, LandsExactlyOnDestinationPose) {
    placeAt(geo(100.0, 30.0, 50000.0), -0.5);

    Viewpoint dest;
    dest.eyeGeo = geo(116.4, 39.9, 20000.0);
    dest.headingRadians = 1.0;
    dest.pitchRadians = -0.8;
    dest.rollRadians = 0.0;

    // 先算出"直接设过去"的位姿作参照 —— 判据是**飞过去和设过去落在同一处**。
    // 这正是 setViewpoint/flyTo 必须共用一份 resolveViewpoint 的原因:两份实现
    // 分岔的表现就是这里差一点,而画面上极难归因。
    const glm::dvec3 eyeBefore = camera_->position().raw();
    system_->setViewpoint(dest);
    const glm::dvec3 expectedEye = camera_->position().raw();
    const glm::dvec3 expectedDir = camera_->direction().raw();
    const glm::dvec3 expectedUp = camera_->up().raw();

    // 回到起点再飞。
    Viewpoint back;
    back.eyeGeo = Ellipsoid::WGS84().cartesianToCartographic(Vec3(eyeBefore));
    system_->setViewpoint(back);

    ASSERT_TRUE(system_->flyTo(dest));
    const int frames = flyToCompletion();
    EXPECT_GT(frames, 1);
    EXPECT_FALSE(system_->cameraFlightActive());

    const double distance = glm::length(expectedEye - eyeBefore);
    const double relativeError =
        glm::length(camera_->position().raw() - expectedEye) / distance;
    EXPECT_LT(relativeError, 1e-3) << "终点位姿相对误差";
    EXPECT_LT(glm::length(camera_->direction().raw() - expectedDir), 1e-9);
    EXPECT_LT(glm::length(camera_->up().raw() - expectedUp), 1e-9);
}

TEST_F(FlightTest, DegenerateFlightLandsImmediatelyWithoutEnteringFlightState) {
    placeAt(geo(100.0, 30.0, 50000.0), -0.5);
    const glm::dvec3 eye0 = camera_->position().raw();

    Viewpoint samePlace;
    samePlace.eyeGeo =
        Ellipsoid::WGS84().cartesianToCartographic(Vec3(eye0));

    // 起终点重合 ⇒ 不进飞行态。否则 isSelfAnimating() 会白撑住几秒重绘,
    // 而画面上什么都没发生。
    EXPECT_FALSE(system_->flyTo(samePlace));
    EXPECT_FALSE(system_->cameraFlightActive());
    EXPECT_LT(glm::length(camera_->position().raw() - eye0), 1e-6);
}

// ============================================================
// 判据 ②:穿山路径全程 AGL ≥ 净空
// ============================================================

TEST_F(FlightTest, FlightOverRidgeKeepsClearanceEveryFrame) {
    // ⚠️ **航程必须短**。拱高取「看见两端」与「地形+净空」的较大者;1500km 航程下
    // 前者约 50km(地球鼓包),把 7km 的山脊整个淹没 ⇒ 地形项根本不参与,测试看着
    // 绿其实一行地形代码都没走到(第一版就是这样,把安全系数归零仍全绿才发现)。
    // ~96km 航程下前者只有约 200m,地形项才是主导项。
    AnalyticRidge ridge;
    // ⚠️ 山脊**不能放在参数中点**。t=0.5 处 sin(πt)=1,逐点反解里那个除以形状的
    // 步骤除不除都一样 ⇒ 「只在中点算一次」这个真 bug 照样绿。放 t≈0.25。
    ridge.ridgeLonDeg = 100.25;
    ridge.halfWidthDeg = 0.2;
    ridge.peakMeters = 7000.0;
    installRidge(ridge);

    // 起终点都远低于山脊顶 ⇒ 沿路径插值必然穿山,只能靠拱高抬起来。
    placeAt(geo(100.0, 30.0, 3000.0), -0.4);
    Viewpoint dest;
    dest.eyeGeo = geo(101.0, 30.0, 3000.0);
    dest.pitchRadians = -0.4;

    ASSERT_TRUE(system_->flyTo(dest));
    const uint64_t clampsBefore = system_->constraintClampCount();

    double minAgl = 1e18;
    double maxHeight = 0.0;
    flyToCompletion([&] {
        minAgl = std::min(minAgl, currentAgl());
        maxHeight = std::max(maxHeight,
                             Ellipsoid::WGS84()
                                 .cartesianToCartographic(camera_->position())
                                 .height());
    });

    // 结果判据:净空是硬不变量,任何控制器都不豁免。
    EXPECT_GE(minAgl, CameraSystem::kMinClearanceMeters)
        << "穿山路径最低 AGL=" << minAgl;

    // ⚠️ **机制判据,缺了上面那条就是空转的**:AGL 达标可以是"路径本来就合法",
    // 也可以是"钳位把相机顶上去了",两者读数完全相同。设计意图是规划期抬拱高
    // 使钳位**结构性不触发**——把拱高强制归零时,上面那条 AGL 断言依然全绿
    // (钳位兜住了),只有这一条会红。
    EXPECT_EQ(clampsBefore, system_->constraintClampCount())
        << "飞行期触发了碰撞钳位 ⇒ 拱高没把路径抬够,是钳位在兜底";

    // 规划确实把路径抬过了山脊,而不是"恰好没穿山所以不用抬":起终点都在
    // 3000m,能爬到山顶以上只能是拱高干的。
    EXPECT_GT(maxHeight, ridge.peakMeters + CameraSystem::kMinClearanceMeters)
        << "路径最高点 " << maxHeight << " 没越过 " << ridge.peakMeters
        << "m 的山脊 ⇒ 拱高没规划出来";
}

TEST_F(FlightTest, ArchHeightIsZeroWhenNoTerrainInjected) {
    // 未注入地形(headless/无瓦片)时不该凭空抬高:那会让短途飞行莫名其妙地
    // 先窜上天。此时拱高只剩"看见两端"那一项,短途下约等于 0。
    placeAt(geo(100.0, 30.0, 10000.0), -0.4);
    Viewpoint dest;
    dest.eyeGeo = geo(100.05, 30.0, 10000.0);

    ASSERT_TRUE(system_->flyTo(dest));
    double maxHeight = 0.0;
    flyToCompletion([&] {
        maxHeight = std::max(maxHeight,
                             Ellipsoid::WGS84()
                                 .cartesianToCartographic(camera_->position())
                                 .height());
    });
    EXPECT_LT(maxHeight, 10000.0 + 500.0) << "短途无地形不该拱起来";
}

// ============================================================
// 判据 ③:isSelfAnimating() 飞行期恒 true
// ============================================================

TEST_F(FlightTest, IsSelfAnimatingHoldsForEveryFrameOfFlight) {
    placeAt(geo(100.0, 30.0, 50000.0), -0.5);
    Viewpoint dest;
    dest.eyeGeo = geo(116.4, 39.9, 20000.0);
    ASSERT_TRUE(system_->flyTo(dest));

    int frames = 0;
    bool everFalse = false;
    while (system_->cameraFlightActive() && frames < 2000) {
        // 漏了这条就是"飞到一半停帧冻住"——与当初漏 pan 惯性同坑。
        if (!system_->isSelfAnimating()) everFalse = true;
        system_->update(kFrameSeconds);
        ++frames;
    }
    EXPECT_GT(frames, 1);
    EXPECT_FALSE(everFalse) << "飞行期出现过 isSelfAnimating()==false";
}

// ============================================================
// 判据 ④:飞行契约喂到 FrameState / 手势抢占
// ============================================================

TEST_F(FlightTest, FlightProgressAdvancesMonotonicallyAndReachesDecelerationBand) {
    placeAt(geo(100.0, 30.0, 50000.0), -0.5);
    Viewpoint dest;
    dest.eyeGeo = geo(116.4, 39.9, 20000.0);
    ASSERT_TRUE(system_->flyTo(dest));

    double last = -1.0;
    bool sawDecelerationBand = false;
    int frames = 0;
    while (system_->cameraFlightActive() && frames < 2000) {
        const double p = system_->cameraFlightProgress();
        EXPECT_GE(p, last) << "进度必须单调";
        EXPECT_GE(p, 0.0);
        EXPECT_LE(p, 1.0);
        last = p;
        // 瓦片侧据 progress > 0.7 关 cullRequestsWhileMoving。进度若永远到不了
        // 那一段,那条豁免就是死代码,而"飞到目的地画面是空的"照旧。
        if (p > 0.7) sawDecelerationBand = true;
        system_->update(kFrameSeconds);
        ++frames;
    }
    EXPECT_TRUE(sawDecelerationBand);
}

TEST_F(FlightTest, GestureTakesOverAndCancelsFlight) {
    placeAt(geo(100.0, 30.0, 50000.0), -0.5);
    Viewpoint dest;
    dest.eyeGeo = geo(116.4, 39.9, 20000.0);
    ASSERT_TRUE(system_->flyTo(dest));
    system_->update(kFrameSeconds);
    ASSERT_TRUE(system_->cameraFlightActive());

    system_->onDragStart(400.0f, 300.0f, 0.0);

    // 手势永远优先(架构 §5)。不取消的话手指在拖、相机却still被程序拽着走。
    EXPECT_FALSE(system_->cameraFlightActive());
    EXPECT_EQ(CameraSystem::kFreeGlobeController,
              system_->activeControllerName());

    // 相机停在取消处,不回弹到起点、也不瞬移到终点。
    const glm::dvec3 eyeAtCancel = camera_->position().raw();
    system_->onDragEnd();
    system_->update(kFrameSeconds);
    EXPECT_LT(glm::length(camera_->position().raw() - eyeAtCancel), 5000.0);
}

TEST_F(FlightTest, SetViewpointDuringFlightAlsoTakesOver) {
    placeAt(geo(100.0, 30.0, 50000.0), -0.5);
    Viewpoint dest;
    dest.eyeGeo = geo(116.4, 39.9, 20000.0);
    ASSERT_TRUE(system_->flyTo(dest));
    system_->update(kFrameSeconds);
    ASSERT_TRUE(system_->cameraFlightActive());

    Viewpoint elsewhere;
    elsewhere.eyeGeo = geo(0.0, 0.0, 100000.0);
    system_->setViewpoint(elsewhere);

    // 显式视角写入也算接管:否则设完位姿飞行还在跑,下一帧就把它拽走了。
    EXPECT_FALSE(system_->cameraFlightActive());
    const Cartographic c =
        Ellipsoid::WGS84().cartesianToCartographic(camera_->position());
    EXPECT_NEAR(c.longitudeDegrees(), 0.0, 1e-6);
    EXPECT_NEAR(c.latitudeDegrees(), 0.0, 1e-6);
}

TEST_F(FlightTest, CancelFlightLeavesCameraWhereItIs) {
    placeAt(geo(100.0, 30.0, 50000.0), -0.5);
    Viewpoint dest;
    dest.eyeGeo = geo(116.4, 39.9, 20000.0);
    ASSERT_TRUE(system_->flyTo(dest));
    for (int i = 0; i < 30; ++i) system_->update(kFrameSeconds);
    const glm::dvec3 mid = camera_->position().raw();

    system_->cancelFlight();

    EXPECT_FALSE(system_->cameraFlightActive());
    EXPECT_FALSE(system_->isSelfAnimating());
    EXPECT_LT(glm::length(camera_->position().raw() - mid), 1e-6);
}

TEST_F(FlightTest, MeasurementFreezeKeepsFlightFromMovingCamera) {
    placeAt(geo(100.0, 30.0, 50000.0), -0.5);
    Viewpoint dest;
    dest.eyeGeo = geo(116.4, 39.9, 20000.0);
    ASSERT_TRUE(system_->flyTo(dest));
    system_->setMeasurementFreeze(true);
    const glm::dvec3 frozen = camera_->position().raw();

    for (int i = 0; i < 60; ++i) system_->update(kFrameSeconds);

    // 覆盖层压过驱动层(架构 §5):冻结时位姿必须逐帧字节稳定,否则北极星测量台
    // 拿到的每一帧都不可复现。
    EXPECT_EQ(frozen.x, camera_->position().raw().x);
    EXPECT_EQ(frozen.y, camera_->position().raw().y);
    EXPECT_EQ(frozen.z, camera_->position().raw().z);
}
