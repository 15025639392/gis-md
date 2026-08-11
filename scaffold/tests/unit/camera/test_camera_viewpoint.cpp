#include <gtest/gtest.h>

#include "earth_engine/camera/CameraPose.h"
#include "earth_engine/camera/CameraSystem.h"
#include "earth_engine/camera/Viewpoint.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"

#include <cmath>
#include <memory>

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;

double wrapToPi(double a) {
    while (a > kPi) a -= 2.0 * kPi;
    while (a < -kPi) a += 2.0 * kPi;
    return a;
}

/// 角度比较要绕圈:heading 0 与 2π 是同一个方向,直接比会假红。
void expectAngleNear(double actual, double expected, double tol,
                     const char* what) {
    EXPECT_NEAR(0.0, wrapToPi(actual - expected), tol) << what;
}

class ViewpointTest : public ::testing::Test {
protected:
    void SetUp() override {
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(45.0 * kPi / 180.0, 1.0, 5.0e7);
        system_ = std::make_unique<CameraSystem>(camera_.get());
        system_->setViewport(800, 600);
    }

    /// 把相机放到一个明确的斜视位姿(不是任何退化点),供各用例作起点。
    void placeOblique() {
        Viewpoint vp;
        vp.targetGeo = Cartographic::fromDegrees(106.5, 29.6, 0.0);
        vp.rangeMeters = 20000.0;
        vp.headingRadians = 0.7;
        vp.pitchRadians = -0.6;
        vp.rollRadians = 0.0;
        system_->setViewpoint(vp);
    }

    std::unique_ptr<Camera> camera_;
    std::unique_ptr<CameraSystem> system_;
};

}  // namespace

// ============================================================
// 判据 ①:setViewpoint → currentViewpoint 往返恒等
// ============================================================

TEST_F(ViewpointTest, RoundTripThroughCurrentViewpointIsPoseIdentity) {
    placeOblique();
    const glm::dvec3 eye0 = camera_->position().raw();
    const glm::dvec3 dir0 = camera_->direction().raw();
    const glm::dvec3 up0 = camera_->up().raw();

    system_->setViewpoint(system_->currentViewpoint());

    // 位姿恒等是**真判据**;字段级恒等只对 eyeGeo 形式成立(见 currentViewpoint
    // 的说明:targetGeo 形式下 hpr 的参考系原点不同)。
    EXPECT_LT(glm::length(camera_->position().raw() - eye0), 1e-6);
    EXPECT_LT(glm::length(camera_->direction().raw() - dir0), 1e-12);
    EXPECT_LT(glm::length(camera_->up().raw() - up0), 1e-12);
}

TEST_F(ViewpointTest, RoundTripIsIdempotentUnderRepetition) {
    placeOblique();
    system_->setViewpoint(system_->currentViewpoint());
    const glm::dvec3 eye1 = camera_->position().raw();
    const glm::dvec3 dir1 = camera_->direction().raw();

    for (int i = 0; i < 20; ++i) {
        system_->setViewpoint(system_->currentViewpoint());
    }

    // 反复往返不该积累漂移——一次恒等但会缓慢爬行的实现,单次用例抓不住。
    EXPECT_LT(glm::length(camera_->position().raw() - eye1), 1e-6);
    EXPECT_LT(glm::length(camera_->direction().raw() - dir1), 1e-12);
}

TEST_F(ViewpointTest, EyeFormRoundTripsAtFieldLevel) {
    Viewpoint vp;
    vp.eyeGeo = Cartographic::fromDegrees(-73.9, 40.7, 5000.0);
    vp.headingRadians = 1.2;
    vp.pitchRadians = -0.35;
    vp.rollRadians = 0.15;
    system_->setViewpoint(vp);

    const Viewpoint got = system_->currentViewpoint();
    ASSERT_TRUE(got.eyeGeo.has_value());
    EXPECT_NEAR(got.eyeGeo->longitude(), vp.eyeGeo->longitude(), 1e-12);
    EXPECT_NEAR(got.eyeGeo->latitude(), vp.eyeGeo->latitude(), 1e-12);
    EXPECT_NEAR(got.eyeGeo->height(), vp.eyeGeo->height(), 1e-6);
    expectAngleNear(*got.headingRadians, 1.2, 1e-12, "heading");
    EXPECT_NEAR(*got.pitchRadians, -0.35, 1e-12);
    expectAngleNear(*got.rollRadians, 0.15, 1e-12, "roll");
}

TEST_F(ViewpointTest, TargetIsNulloptWhenLookingAtSky) {
    Viewpoint vp;
    vp.eyeGeo = Cartographic::fromDegrees(106.5, 29.6, 100000.0);
    vp.pitchRadians = 0.6;  // 仰视
    system_->setViewpoint(vp);

    const Viewpoint got = system_->currentViewpoint();
    // 诚实表达"当前没有焦点"。伪造一个地平线外的假焦点会让上层(飞行插值、
    // 双击定位)拿到一个物理上不存在的点还以为是真的。
    EXPECT_FALSE(got.targetGeo.has_value());
    EXPECT_FALSE(got.rangeMeters.has_value());
    // 但 eye/hpr 恒可解。
    EXPECT_TRUE(got.eyeGeo.has_value());
    EXPECT_TRUE(got.headingRadians.has_value());
    EXPECT_TRUE(got.pitchRadians.has_value());
    EXPECT_TRUE(got.rollRadians.has_value());
}

// ============================================================
// 判据 ②:hpr 单独写入互不串扰
// ============================================================

TEST_F(ViewpointTest, WritingHeadingLeavesPitchRollAndEyeUntouched) {
    placeOblique();
    const Viewpoint before = system_->currentViewpoint();
    const glm::dvec3 eye0 = camera_->position().raw();

    Viewpoint only;
    only.headingRadians = 0.0;
    system_->setViewpoint(only);

    const Viewpoint after = system_->currentViewpoint();
    expectAngleNear(*after.headingRadians, 0.0, 1e-9, "heading 已写入");
    EXPECT_NEAR(*after.pitchRadians, *before.pitchRadians, 1e-9)
        << "pitch 被 heading 写入带偏 = 串扰";
    expectAngleNear(*after.rollRadians, *before.rollRadians, 1e-9, "roll 串扰");
    // 纯朝向写入 ⇒ range=0 ⇒ 绕相机自身原地转,eye 不动(= resetNorthUp 语义)。
    EXPECT_LT(glm::length(camera_->position().raw() - eye0), 1e-6);
}

TEST_F(ViewpointTest, WritingPitchLeavesHeadingRollAndEyeUntouched) {
    placeOblique();
    const Viewpoint before = system_->currentViewpoint();
    const glm::dvec3 eye0 = camera_->position().raw();

    Viewpoint only;
    only.pitchRadians = -0.2;
    system_->setViewpoint(only);

    const Viewpoint after = system_->currentViewpoint();
    EXPECT_NEAR(*after.pitchRadians, -0.2, 1e-9);
    expectAngleNear(*after.headingRadians, *before.headingRadians, 1e-9,
                    "heading 串扰");
    expectAngleNear(*after.rollRadians, *before.rollRadians, 1e-9, "roll 串扰");
    EXPECT_LT(glm::length(camera_->position().raw() - eye0), 1e-6);
}

TEST_F(ViewpointTest, WritingRollLeavesHeadingPitchAndEyeUntouched) {
    placeOblique();
    const Viewpoint before = system_->currentViewpoint();
    const glm::dvec3 eye0 = camera_->position().raw();

    Viewpoint only;
    only.rollRadians = 0.4;
    system_->setViewpoint(only);

    const Viewpoint after = system_->currentViewpoint();
    expectAngleNear(*after.rollRadians, 0.4, 1e-9, "roll 已写入");
    expectAngleNear(*after.headingRadians, *before.headingRadians, 1e-9,
                    "heading 串扰");
    EXPECT_NEAR(*after.pitchRadians, *before.pitchRadians, 1e-9)
        << "pitch 串扰";
    EXPECT_LT(glm::length(camera_->position().raw() - eye0), 1e-6);
}

TEST_F(ViewpointTest, RangeOnlyWriteKeepsOrientation) {
    placeOblique();
    const Viewpoint before = system_->currentViewpoint();
    ASSERT_TRUE(before.targetGeo.has_value());
    const glm::dvec3 dir0 = camera_->direction().raw();

    Viewpoint only;
    only.targetGeo = before.targetGeo;
    only.rangeMeters = 50000.0;
    system_->setViewpoint(only);

    const Viewpoint after = system_->currentViewpoint();
    ASSERT_TRUE(after.rangeMeters.has_value());
    EXPECT_NEAR(*after.rangeMeters, 50000.0, 1.0);
    // 只改距离不该改朝向。**判据落在视线向量上而不是 heading 读数上**:hpr 按
    // eye 的 ENU 报,相机沿视线退远 30km 后 eye 的 ENU 本就转过一点,拿读数比
    // 会把"球面上 ENU 逐点转动"这个几何事实误判成朝向被改。视线向量没有这个
    // 参考系问题,而且判据更强(逐位)。
    EXPECT_LT(glm::length(camera_->direction().raw() - dir0), 1e-12);
}

// ============================================================
// CameraPose 本身:互转与退化
// ============================================================

TEST(CameraPoseTest, FromFrameToFrameRoundTripsAwayFromGimbal) {
    const glm::dvec3 origin =
        Ellipsoid::WGS84()
            .cartographicToCartesian(Cartographic::fromDegrees(12.0, 45.0, 0.0))
            .raw();
    const glm::dmat3 frame = CameraPose::enuFrameAt(origin);

    const double h = 2.3, p = -0.4, r = 0.25, range = 1234.5;
    const CameraPose pose = CameraPose::fromFrame(origin, frame, h, p, r, range);

    double h2, p2, r2, range2;
    pose.toFrame(origin, frame, h2, p2, r2, range2);

    expectAngleNear(h2, h, 1e-12, "heading");
    EXPECT_NEAR(p2, p, 1e-12);
    expectAngleNear(r2, r, 1e-12, "roll");
    EXPECT_NEAR(range2, range, 1e-9);
}

TEST(CameraPoseTest, HeadingZeroPointsNorthAndPositiveTurnsEast) {
    const glm::dvec3 origin =
        Ellipsoid::WGS84()
            .cartographicToCartesian(Cartographic::fromDegrees(0.0, 0.0, 0.0))
            .raw();
    const glm::dmat3 frame = CameraPose::enuFrameAt(origin);
    const glm::dvec3 east = frame[0];
    const glm::dvec3 north = frame[1];

    const CameraPose n = CameraPose::fromFrame(origin, frame, 0.0, 0.0, 0.0, 0.0);
    EXPECT_GT(glm::dot(n.direction, north), 0.999999);

    const CameraPose e =
        CameraPose::fromFrame(origin, frame, kPi / 2.0, 0.0, 0.0, 0.0);
    // heading 顺时针为正 ⇒ +90° 看向正东。搞反的话指北针会镜像,而静止画面上
    // 完全看不出来。
    EXPECT_GT(glm::dot(e.direction, east), 0.999999);
}

TEST(CameraPoseTest, NadirGimbalResolvesHeadingFromUpAndZeroesRoll) {
    const glm::dvec3 origin =
        Ellipsoid::WGS84()
            .cartographicToCartesian(Cartographic::fromDegrees(30.0, -20.0, 0.0))
            .raw();
    const glm::dmat3 frame = CameraPose::enuFrameAt(origin);

    // 正俯视:direction 沿天底,绕它转不改 direction ⇒ heading 只能由 up 定。
    const double h = 1.1;
    const CameraPose pose =
        CameraPose::fromFrame(origin, frame, h, -kPi / 2.0, 0.0, 500.0);

    double h2, p2, r2, range2;
    pose.toFrame(origin, frame, h2, p2, r2, range2);

    EXPECT_NEAR(p2, -kPi / 2.0, 1e-9);
    expectAngleNear(h2, h, 1e-9, "正俯视下 heading 应由 up 恢复");
    EXPECT_NEAR(r2, 0.0, 1e-12) << "万向节区约定 roll=0";

    // 再走一遍 fromFrame 必须回到同一位姿——这才是万向节兜底"够用"的判据:
    // (heading, roll) 的分解不唯一,但位姿必须唯一。
    const CameraPose again =
        CameraPose::fromFrame(origin, frame, h2, p2, r2, range2);
    EXPECT_LT(glm::length(again.direction - pose.direction), 1e-12);
    EXPECT_LT(glm::length(again.up - pose.up), 1e-12);
    EXPECT_LT(glm::length(again.eye - pose.eye), 1e-9);
}

TEST(CameraPoseTest, ZenithGimbalAlsoRoundTripsPose) {
    const glm::dvec3 origin =
        Ellipsoid::WGS84()
            .cartographicToCartesian(Cartographic::fromDegrees(-5.0, 60.0, 0.0))
            .raw();
    const glm::dmat3 frame = CameraPose::enuFrameAt(origin);

    const CameraPose pose =
        CameraPose::fromFrame(origin, frame, 2.0, kPi / 2.0, 0.0, 100.0);
    double h2, p2, r2, range2;
    pose.toFrame(origin, frame, h2, p2, r2, range2);
    const CameraPose again =
        CameraPose::fromFrame(origin, frame, h2, p2, r2, range2);

    EXPECT_LT(glm::length(again.direction - pose.direction), 1e-12);
    EXPECT_LT(glm::length(again.up - pose.up), 1e-12);
}

TEST(CameraPoseTest, EnuFrameIsRightHandedOrthonormal) {
    for (double lat : {-89.0, -45.0, 0.0, 37.0, 89.0}) {
        const glm::dvec3 origin =
            Ellipsoid::WGS84()
                .cartographicToCartesian(
                    Cartographic::fromDegrees(21.0, lat, 0.0))
                .raw();
        const glm::dmat3 f = CameraPose::enuFrameAt(origin);
        EXPECT_NEAR(glm::length(f[0]), 1.0, 1e-12) << "lat=" << lat;
        EXPECT_NEAR(glm::length(f[1]), 1.0, 1e-12) << "lat=" << lat;
        EXPECT_NEAR(glm::length(f[2]), 1.0, 1e-12) << "lat=" << lat;
        EXPECT_NEAR(glm::dot(f[0], f[1]), 0.0, 1e-12) << "lat=" << lat;
        EXPECT_NEAR(glm::dot(f[1], f[2]), 0.0, 1e-12) << "lat=" << lat;
        // 右手:east × north = up。反了的话 heading 的旋向整体翻转。
        EXPECT_GT(glm::dot(glm::cross(f[0], f[1]), f[2]), 0.999999)
            << "lat=" << lat;
    }
}

// ============================================================
// 判据 ③:与既有只读派生量同源(不能出现两套 heading 定义)
// ============================================================

TEST_F(ViewpointTest, CurrentViewpointAgreesWithLegacyHeadingPitchAccessors) {
    placeOblique();
    const Viewpoint vp = system_->currentViewpoint();

    // headingRadians()/pitchRadians() 是已上线读数(指北针在用)。它们与
    // currentViewpoint 分岔的话,界面上的指北针和 API 报的朝向会对不上。
    expectAngleNear(*vp.headingRadians, system_->headingRadians(), 1e-12,
                    "heading 与既有访问器同源");
    EXPECT_NEAR(*vp.pitchRadians, system_->pitchRadians(), 1e-12);
}
