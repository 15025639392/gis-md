#include <gtest/gtest.h>

#include "earth_engine/camera/CameraPose.h"
#include "earth_engine/camera/CameraSystem.h"
#include "earth_engine/camera/Viewpoint.h"
#include "earth_engine/camera/controllers/TetheredController.h"
#include "earth_engine/core/geodesy/Cartographic.h"
#include "earth_engine/core/geodesy/Ellipsoid.h"
#include "earth_engine/scene/Camera.h"

#include <cmath>
#include <memory>

using namespace earth_engine;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFrameSeconds = 1.0 / 60.0;

/// 可移动、可改姿态的假载体。测试全靠它驱动 provider。
struct FakeCarrier {
    glm::dvec3 position{0.0};
    glm::dmat3 orientation{1.0};
    bool available = true;

    void placeGeo(double lonDeg, double latDeg, double h) {
        position = Ellipsoid::WGS84()
                       .cartographicToCartesian(
                           Cartographic::fromDegrees(lonDeg, latDeg, h))
                       .raw();
        orientation = CameraPose::enuFrameAt(position);
    }
    /// 绕自身"上"轴转 yaw、再绕"前"轴 roll —— 模拟载体姿态变化。
    void setAttitude(double yaw, double roll) {
        const glm::dmat3 enu = CameraPose::enuFrameAt(position);
        const glm::dquat q = glm::angleAxis(-yaw, enu[2]) *
                             glm::angleAxis(roll, enu[1]);
        orientation = glm::dmat3(q * enu[0], q * enu[1], q * enu[2]);
    }
};

class TetheredTest : public ::testing::Test {
protected:
    void SetUp() override {
        camera_ = std::make_unique<Camera>();
        camera_->setPerspective(45.0 * kPi / 180.0, 1.0, 5.0e7);
        system_ = std::make_unique<CameraSystem>(camera_.get());
        system_->setViewport(800, 600);
        carrier_.placeGeo(106.5, 29.6, 3000.0);
    }

    /// 只接 originProvider(跟车但保持北上)。
    ViewpointFrame positionOnlyFrame() {
        ViewpointFrame f;
        FakeCarrier* c = &carrier_;
        f.originProvider = [c](glm::dvec3& out) {
            if (!c->available) return false;
            out = c->position;
            return true;
        };
        return f;
    }

    /// 接上 orientationProvider(完全固连机体系,roll 跟随)。
    ViewpointFrame fullFrame() {
        ViewpointFrame f = positionOnlyFrame();
        FakeCarrier* c = &carrier_;
        f.orientationProvider = [c](glm::dmat3& out) {
            if (!c->available) return false;
            out = c->orientation;
            return true;
        };
        return f;
    }

    TetheredController& tethered() { return system_->tetheredController(); }

    /// ⚠️ 顺序要紧:`onActivate` 会**从当前世界位姿重算全部真值**,所以
    /// setRange/setLocalOrientation 必须在 select 之后,否则被静默覆盖。
    void attachAt(const ViewpointFrame& frame, double range) {
        tethered().setFrame(frame);
        system_->selectController(CameraSystem::kTetheredController);
        system_->update(kFrameSeconds);
        tethered().setRange(range);
        system_->update(kFrameSeconds);
    }

    FakeCarrier carrier_;
    std::unique_ptr<Camera> camera_;
    std::unique_ptr<CameraSystem> system_;
};

}  // namespace

// ============================================================
// 判据 ①:载体移动 1000 km 后相对位姿(localHPR + range)逐位不变
// ============================================================

TEST_F(TetheredTest, RelativePoseIsBitExactAfterCarrierTravels1000km) {
    attachAt(positionOnlyFrame(), 500.0);
    tethered().setLocalOrientation(0.9, -0.4, 0.0);
    system_->update(kFrameSeconds);

    const double h0 = tethered().localHeading();
    const double p0 = tethered().localPitch();
    const double r0 = tethered().localRoll();
    const double range0 = tethered().range();
    const glm::dvec3 eye0 = camera_->position().raw();
    // 相机相对载体的世界偏移(载体系下应当是不变量)。
    const glm::dvec3 offset0 = eye0 - carrier_.position;

    // 载体沿经线走 ~1000km 并抬升。
    for (int i = 1; i <= 90; ++i) {
        carrier_.placeGeo(106.5, 29.6 + 9.0 * i / 90.0, 3000.0 + 20.0 * i);
        system_->update(kFrameSeconds);
    }

    // **逐位不变**:真值是 (frame, localHPR, range),载体移动完全不该动它。
    // 若哪天有人把真值改成世界位姿,这四个数会随载体一起漂 —— 那正是"脱钩"。
    EXPECT_EQ(h0, tethered().localHeading());
    EXPECT_EQ(p0, tethered().localPitch());
    EXPECT_EQ(r0, tethered().localRoll());
    EXPECT_EQ(range0, tethered().range());

    // 世界位姿则**必须**跟着走(它是派生量)。
    EXPECT_GT(glm::length(camera_->position().raw() - eye0), 9.0e5);
    // 到载体的距离仍是 range(相对几何守住了)。
    EXPECT_NEAR(glm::length(camera_->position().raw() - carrier_.position),
                range0, 1e-6);
    // 偏移向量的模不变(方向会随 ENU 在球面上转,这是对的)。
    EXPECT_NEAR(glm::length(camera_->position().raw() - carrier_.position),
                glm::length(offset0), 1e-6);
}

TEST_F(TetheredTest, CarrierStaysCenteredInViewWhileMoving) {
    attachAt(positionOnlyFrame(), 800.0);
    tethered().setLocalOrientation(1.3, -0.3, 0.0);
    system_->update(kFrameSeconds);

    for (int i = 1; i <= 60; ++i) {
        carrier_.placeGeo(106.5 + 0.05 * i, 29.6, 3000.0);
        system_->update(kFrameSeconds);
        // 视线必须始终精确指向载体 —— 这是 tether 最基本的观感不变量,
        // 而它在"真值是世界位姿"的实现里恰恰守不住(载体动了位姿不动)。
        const glm::dvec3 toCarrier =
            glm::normalize(carrier_.position - camera_->position().raw());
        EXPECT_GT(glm::dot(toCarrier, camera_->direction().raw()), 1.0 - 1e-12)
            << "第 " << i << " 帧视线偏离载体";
    }
}

// ============================================================
// 判据 ②:Free ↔ Tethered 切换帧位姿连续(跳变 < 1e-6 m)
// ============================================================

TEST_F(TetheredTest, SwitchingFreeToTetheredIsPoseContinuous) {
    // 先在 Free 下摆一个明确的斜视位姿。
    // Free 位姿**正看着载体**(系留的正常用法)。此时位置与朝向同时保住。
    Viewpoint vp;
    vp.targetGeo = Cartographic::fromDegrees(106.5, 29.6, 3000.0);
    vp.rangeMeters = 4000.0;
    vp.headingRadians = 0.6;
    vp.pitchRadians = -0.5;
    system_->setViewpoint(vp);
    system_->update(kFrameSeconds);

    const glm::dvec3 eyeBefore = camera_->position().raw();
    const glm::dvec3 dirBefore = camera_->direction().raw();
    const glm::dvec3 upBefore = camera_->up().raw();

    tethered().setFrame(positionOnlyFrame());
    system_->selectController(CameraSystem::kTetheredController);
    system_->update(kFrameSeconds);

    // 零跳变的全部机制 = onActivate 把当前世界位姿换算成相对位姿。
    // 载体没动,所以换算回来必须还是原位姿。
    EXPECT_LT(glm::length(camera_->position().raw() - eyeBefore), 1e-6);
    EXPECT_LT(glm::length(camera_->direction().raw() - dirBefore), 1e-9);
    EXPECT_LT(glm::length(camera_->up().raw() - upBefore), 1e-9);
    EXPECT_TRUE(tethered().frameResolved());
}

TEST_F(TetheredTest, SwitchingTetheredToFreeIsPoseContinuous) {
    attachAt(positionOnlyFrame(), 1200.0);
    tethered().setLocalOrientation(2.1, -0.7, 0.0);
    for (int i = 0; i < 10; ++i) {
        carrier_.placeGeo(106.5 + 0.01 * i, 29.6, 3000.0);
        system_->update(kFrameSeconds);
    }
    const glm::dvec3 eyeBefore = camera_->position().raw();
    const glm::dvec3 dirBefore = camera_->direction().raw();

    system_->selectController(CameraSystem::kFreeGlobeController);
    system_->update(kFrameSeconds);

    // 回到 Free 也不能跳:Free 的真值本就是世界位姿,接管 = 什么都不用换算,
    // 但**不能顺手把位姿重置**(FreeGlobeController::onActivate 只清瞬时量)。
    EXPECT_LT(glm::length(camera_->position().raw() - eyeBefore), 1e-6);
    EXPECT_LT(glm::length(camera_->direction().raw() - dirBefore), 1e-9);
}

TEST_F(TetheredTest, RoundTripFreeTetheredFreeIsPoseContinuous) {
    Viewpoint vp;
    vp.targetGeo = Cartographic::fromDegrees(106.5, 29.6, 3000.0);
    vp.rangeMeters = 9000.0;
    vp.headingRadians = 1.7;
    vp.pitchRadians = -0.25;
    system_->setViewpoint(vp);
    system_->update(kFrameSeconds);
    const glm::dvec3 eye0 = camera_->position().raw();
    const glm::dvec3 dir0 = camera_->direction().raw();
    const glm::dvec3 up0 = camera_->up().raw();

    tethered().setFrame(positionOnlyFrame());
    system_->selectController(CameraSystem::kTetheredController);
    system_->update(kFrameSeconds);
    system_->selectController(CameraSystem::kFreeGlobeController);
    system_->update(kFrameSeconds);

    // 载体静止时来回切必须是恒等 —— 任何一侧的换算有偏差都会在这里累积。
    EXPECT_LT(glm::length(camera_->position().raw() - eye0), 1e-6);
    EXPECT_LT(glm::length(camera_->direction().raw() - dir0), 1e-9);
    EXPECT_LT(glm::length(camera_->up().raw() - up0), 1e-9);
}

TEST_F(TetheredTest, AttachingWhileLookingAwayKeepsPositionAndReAims) {
    // 相机在载体附近但**看着别处**。系留真值是 orbit 表述
    // (eye = origin − direction·range),表达不了这种位姿 ⇒ 位置与朝向只能保一个。
    Viewpoint vp;
    vp.eyeGeo = Cartographic::fromDegrees(106.6, 29.7, 9000.0);
    vp.headingRadians = 2.9;   // 背对载体
    vp.pitchRadians = 0.2;
    system_->setViewpoint(vp);
    system_->update(kFrameSeconds);
    const glm::dvec3 eyeBefore = camera_->position().raw();
    const glm::dvec3 dirBefore = camera_->direction().raw();

    tethered().setFrame(positionOnlyFrame());
    system_->selectController(CameraSystem::kTetheredController);
    system_->update(kFrameSeconds);

    // 位置精确保留(判据说的是"跳变 < 1e-6 米")。
    EXPECT_LT(glm::length(camera_->position().raw() - eyeBefore), 1e-6);
    // 视线转向载体 —— 这是模型的固有限制,**不是 bug**,写成用例是为了让它
    // 被显式记录:哪天有人"修"成保朝向,位置就会跳。
    const glm::dvec3 toCarrier =
        glm::normalize(carrier_.position - camera_->position().raw());
    EXPECT_GT(glm::dot(toCarrier, camera_->direction().raw()), 1.0 - 1e-12);
    EXPECT_LT(glm::dot(dirBefore, camera_->direction().raw()), 0.99);
}

// ============================================================
// 判据 ③:载体姿态变化时 roll 跟随(orientationProvider 用例)
// ============================================================

TEST_F(TetheredTest, RollFollowsCarrierWhenOrientationProviderPresent) {
    attachAt(fullFrame(), 0.0);            // 座舱视角:range=0
    tethered().setLocalOrientation(0.0, 0.0, 0.0);
    system_->update(kFrameSeconds);
    const glm::dvec3 upLevel = camera_->up().raw();

    // 载体横滚 30°。
    carrier_.setAttitude(0.0, 30.0 * kPi / 180.0);
    system_->update(kFrameSeconds);
    const glm::dvec3 upRolled = camera_->up().raw();

    const double tilt = std::acos(
        std::clamp(glm::dot(glm::normalize(upLevel), glm::normalize(upRolled)),
                   -1.0, 1.0));
    // localRoll 一动没动,画面却横滚了 —— roll 是从载体机体系继承来的。
    EXPECT_EQ(0.0, tethered().localRoll());
    EXPECT_NEAR(tilt, 30.0 * kPi / 180.0, 1e-9) << "roll 没跟随载体";
}

TEST_F(TetheredTest, RollDoesNotFollowWhenOnlyOriginProviderPresent) {
    attachAt(positionOnlyFrame(), 0.0);    // 跟车但保持北上
    tethered().setLocalOrientation(0.0, 0.0, 0.0);
    system_->update(kFrameSeconds);
    const glm::dvec3 upLevel = camera_->up().raw();

    carrier_.setAttitude(0.0, 30.0 * kPi / 180.0);
    system_->update(kFrameSeconds);

    // 三档表格的中间那档:orientationProvider 空 ⇒ 用地理 ENU ⇒ 载体怎么翻滚
    // 画面都保持北上。这条与上一条是**同一份代码的两个分支**,缺一条就分不清
    // "roll 跟随"是真的跟随还是恒等于载体姿态。
    const double tilt = std::acos(std::clamp(
        glm::dot(glm::normalize(upLevel), glm::normalize(camera_->up().raw())),
        -1.0, 1.0));
    EXPECT_NEAR(tilt, 0.0, 1e-6) << "只给 origin 时不该跟随载体姿态";
}

// ============================================================
// 生命周期 / 边界
// ============================================================

TEST_F(TetheredTest, UnavailableCarrierHoldsPoseInsteadOfFallingBackToWorld) {
    attachAt(positionOnlyFrame(), 600.0);
    system_->update(kFrameSeconds);
    const glm::dvec3 held = camera_->position().raw();

    carrier_.available = false;            // 载体销毁/尚未生成
    carrier_.placeGeo(0.0, 0.0, 0.0);      // 即便"位置"变了也不该被用
    for (int i = 0; i < 30; ++i) system_->update(kFrameSeconds);

    // 回落世界系会是一次瞬移(相机被甩到几内亚湾)。保持上帧才对。
    EXPECT_LT(glm::length(camera_->position().raw() - held), 1e-6);
    EXPECT_FALSE(tethered().frameResolved());
    EXPECT_FALSE(system_->isSelfAnimating());
}

TEST_F(TetheredTest, IsSelfAnimatingTracksCarrierMotionNotConstantTrue) {
    attachAt(positionOnlyFrame(), 600.0);
    system_->update(kFrameSeconds);
    system_->update(kFrameSeconds);

    // 载体静止 ⇒ 必须能空闲。恒 true 会让系留相机永远不停帧(按需渲染失效)。
    EXPECT_FALSE(system_->isSelfAnimating());

    carrier_.placeGeo(106.6, 29.6, 3000.0);
    system_->update(kFrameSeconds);
    // 载体动了 ⇒ 还得继续画。恒 false 会让画面停在半路。
    EXPECT_TRUE(system_->isSelfAnimating());

    system_->update(kFrameSeconds);
    EXPECT_FALSE(system_->isSelfAnimating());
}

TEST_F(TetheredTest, DragOrbitsCarrierKeepingRangeAndCentering) {
    attachAt(positionOnlyFrame(), 2000.0);
    tethered().setLocalOrientation(0.0, -0.3, 0.0);
    system_->update(kFrameSeconds);
    const double headingBefore = tethered().localHeading();

    system_->onDragStart(400.0f, 300.0f, 0.0);
    system_->onDragMove(500.0f, 300.0f, 0.016);
    system_->onDragEnd();
    system_->update(kFrameSeconds);

    // 手指右移 ⇒ heading 减小(与 Free 侧"手指右移世界右转"同取向)。
    // ⚠️ 增益与手感须真机验;host 只钉符号 —— 符号写反在静止截图上完全看不出来。
    EXPECT_LT(tethered().localHeading(), headingBefore);
    // 绕载体转:距离不变、载体仍在视线上。
    EXPECT_NEAR(tethered().range(), 2000.0, 1e-9);
    EXPECT_NEAR(glm::length(camera_->position().raw() - carrier_.position),
                2000.0, 1e-6);
    const glm::dvec3 toCarrier =
        glm::normalize(carrier_.position - camera_->position().raw());
    EXPECT_GT(glm::dot(toCarrier, camera_->direction().raw()), 1.0 - 1e-12);
}

TEST_F(TetheredTest, PinchChangesRangeOnly) {
    attachAt(positionOnlyFrame(), 2000.0);
    tethered().setLocalOrientation(0.5, -0.4, 0.0);
    system_->update(kFrameSeconds);
    const double h0 = tethered().localHeading();
    const double p0 = tethered().localPitch();

    CameraSystem::PinchInput in;
    in.scaleFromStart = 1.25f;   // 捏开 = 拉近
    in.centroidX = 400.0f;
    in.centroidY = 300.0f;
    in.timestamp = 0.016;
    system_->onPinchGesture(in);
    system_->onPinchEnd();
    system_->update(kFrameSeconds);

    EXPECT_LT(tethered().range(), 2000.0) << "捏开应拉近";
    EXPECT_EQ(h0, tethered().localHeading()) << "捏合不该改朝向";
    EXPECT_EQ(p0, tethered().localPitch());
}

TEST_F(TetheredTest, ClearanceStillAppliesButTruthStaysUnclamped) {
    // 载体贴地,相机 range=0 ⇒ 世界位姿会跌破净空。
    carrier_.placeGeo(106.5, 29.6, 0.0);
    attachAt(positionOnlyFrame(), 0.0);
    tethered().setLocalOrientation(0.0, 0.0, 0.0);
    const double rangeTruth = tethered().range();
    system_->update(kFrameSeconds);

    // 净空是硬不变量,系留也不豁免 —— 帧末哨兵把渲染位姿顶上去。
    const double h = Ellipsoid::WGS84()
                         .cartesianToCartographic(camera_->position())
                         .height();
    EXPECT_GE(h, CameraSystem::kMinClearanceMeters - 1e-6);

    // 但**真值不被钳位污染**:载体升空后相机精确回到原来的相对位姿,
    // 不留下被地形推走的欠账。改成"钳完写回真值"就会在这里露馅。
    EXPECT_EQ(rangeTruth, tethered().range());
    carrier_.placeGeo(106.5, 29.6, 20000.0);
    system_->update(kFrameSeconds);
    EXPECT_NEAR(glm::length(camera_->position().raw() - carrier_.position),
                rangeTruth, 1e-6);
}
