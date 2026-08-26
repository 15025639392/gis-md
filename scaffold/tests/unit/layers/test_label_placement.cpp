#include <gtest/gtest.h>

#include "earth_engine/layers/LabelPlacement.h"
#include "earth_engine/scene/Camera.h"

#include <cmath>

using namespace earth_engine;

namespace {

/// 单位球场景(radii=1,便于手算地平线几何):相机在 (2,0,0) 看原点。
/// 地平圆:cosθ = 1/2 → θ=60°(与 +x 轴夹角)。
class LabelPlacementTest : public ::testing::Test {
protected:
    LabelPlacement::FrameInput makeInput(const Vec3& eye, const Vec3& target) {
        Camera cam;
        cam.lookAt(eye, target, Vec3(0.0, 0.0, 1.0));
        LabelPlacement::FrameInput in;
        in.viewProj = cam.viewProjectionMatrix(800.0, 600.0);
        in.cameraEcef = eye;
        in.ellipsoidRadii = Vec3(1.0, 1.0, 1.0);
        in.viewportWidthPx = 800;
        in.viewportHeightPx = 600;
        in.deltaSeconds = 1.0;  // 一帧收敛(> kFadeSeconds)
        return in;
    }

    static LabelCandidate makeCandidate(FeatureId id, const Vec3& anchor,
                                        float halfW = 40.0f,
                                        float halfH = 15.0f,
                                        int rank = 6) {
        LabelCandidate c;
        c.featureId = id;
        c.rank = rank;
        c.anchorEcef = anchor;
        c.boxMinXPx = -halfW;
        c.boxMaxXPx = halfW;
        c.boxMinYPx = -halfH;
        c.boxMaxYPx = halfH;
        return c;
    }

    LabelPlacement placement;
};

/// 与 +x 轴夹角 θ(度) 的单位球赤道面点。
Vec3 spherePoint(double thetaDeg) {
    const double t = thetaDeg * M_PI / 180.0;
    return Vec3(std::cos(t), std::sin(t), 0.0);
}

} // namespace

TEST_F(LabelPlacementTest, SingleVisibleLabelPlacedAndConverges) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const std::vector<LabelCandidate> cands = {
        makeCandidate(1, spherePoint(0.0))};

    EXPECT_TRUE(placement.update(in, cands));  // fade 推进 = 有变化
    EXPECT_EQ(1, placement.stats().placed);
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));

    // 已收敛:再跑无变化。
    EXPECT_FALSE(placement.update(in, cands));
}

// V27:hasPendingFades 是"帧循环还得续帧"的判据 —— 它一旦漏报,冷启动瓦片
// 加载完就停帧、标注 fade 冻在半程 = POI 首现要缩放催化的根因。钉死:半程时
// 报 true,收敛后报 false;空态 false。
TEST_F(LabelPlacementTest, HasPendingFadesGatesFrameContinuation) {
    LabelPlacement fresh;
    EXPECT_FALSE(fresh.hasPendingFades()) << "空态无待收敛 fade";

    auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    in.deltaSeconds = 0.1;  // fade 0.3s → 每帧 1/3,首帧后停在 0.333(半程)
    const std::vector<LabelCandidate> cands = {
        makeCandidate(1, spherePoint(0.0))};

    fresh.update(in, cands);
    EXPECT_NEAR(0.333f, fresh.opacity(1), 0.01f);
    EXPECT_TRUE(fresh.hasPendingFades()) << "半程:必须报 true,否则帧停 fade 冻死";

    fresh.update(in, cands);  // 0.667
    EXPECT_TRUE(fresh.hasPendingFades());
    fresh.update(in, cands);  // 1.0 收敛
    EXPECT_FLOAT_EQ(1.0f, fresh.opacity(1));
    EXPECT_FALSE(fresh.hasPendingFades()) << "收敛后:报 false,续帧即止不空烧";
}

TEST_F(LabelPlacementTest, FadeIsGradualWithSmallDelta) {
    auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    in.deltaSeconds = 0.1;  // fade 0.3s → 每帧 1/3
    const std::vector<LabelCandidate> cands = {
        makeCandidate(1, spherePoint(0.0))};

    placement.update(in, cands);
    EXPECT_NEAR(0.333f, placement.opacity(1), 0.01f);
    placement.update(in, cands);
    EXPECT_NEAR(0.667f, placement.opacity(1), 0.01f);
    placement.update(in, cands);
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));
}

TEST_F(LabelPlacementTest, OverlapLowerIdWinsOnDistanceTie) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const Vec3 p = spherePoint(0.0);
    const std::vector<LabelCandidate> cands = {makeCandidate(2, p),
                                               makeCandidate(1, p)};

    placement.update(in, cands);
    EXPECT_EQ(1, placement.stats().placed);
    EXPECT_EQ(1, placement.stats().collided);
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(2));
}

TEST_F(LabelPlacementTest, HigherImportanceRankWinsCollisionBeforeDistance) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    // Amap rank is normalized at the adapter boundary so that a smaller
    // generic rank is more important.  Keep ids and distance deliberately
    // unfavorable to prove rank is an independent collision tiebreaker.
    const std::vector<LabelCandidate> cands = {
        makeCandidate(99, spherePoint(20.0), 400.0f, 300.0f, -7),
        makeCandidate(1, spherePoint(0.0), 400.0f, 300.0f, 5)};

    placement.update(in, cands);
    EXPECT_EQ(1, placement.stats().placed);
    EXPECT_EQ(1, placement.stats().collided);
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(99));
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(1));
}

TEST_F(LabelPlacementTest, CloserLabelWinsCollision) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    // 大盒强制两点屏幕重叠:近者(id 大)按距离优先赢。
    const std::vector<LabelCandidate> wide = {
        makeCandidate(1, spherePoint(20.0), 400.0f, 300.0f),
        makeCandidate(9, spherePoint(0.0), 400.0f, 300.0f)};

    placement.update(in, wide);
    EXPECT_EQ(1, placement.stats().placed);
    EXPECT_EQ(1, placement.stats().collided);
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(9));  // 距相机更近
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(1));
}

TEST_F(LabelPlacementTest, PriorityFeatureBeatsDistance) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const std::vector<LabelCandidate> wide = {
        makeCandidate(1, spherePoint(20.0), 400.0f, 300.0f),
        makeCandidate(9, spherePoint(0.0), 400.0f, 300.0f)};

    placement.setPriorityFeature(1);
    placement.update(in, wide);
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(9));
}

TEST_F(LabelPlacementTest, BehindCameraCulled) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    // 相机身后(x>2)。
    const std::vector<LabelCandidate> cands = {
        makeCandidate(1, Vec3(3.0, 0.0, 0.0))};

    placement.update(in, cands);
    EXPECT_EQ(1, placement.stats().culledProjection);
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(1));
}

TEST_F(LabelPlacementTest, BackSideOfGlobeHorizonCulled) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    // 球背面点 (-1,0,0):在视锥中央(穿过球心)但被椭球遮挡。
    const std::vector<LabelCandidate> cands = {
        makeCandidate(1, spherePoint(180.0))};

    placement.update(in, cands);
    EXPECT_EQ(1, placement.stats().culledHorizon);
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(1));
}

TEST_F(LabelPlacementTest, NearHorizonFadesPartially) {
    // 地平圆在 θ=60°;θ=59.5° 可见但已进 fade band → 目标 opacity ∈ (0,1)。
    // 相机直视该点保证投影在屏内。
    const Vec3 p = spherePoint(59.5);
    const auto in = makeInput(Vec3(2, 0, 0), p);
    const std::vector<LabelCandidate> cands = {makeCandidate(1, p)};

    placement.update(in, cands);
    ASSERT_EQ(1, placement.stats().placed);
    const float fadeTarget = placement.opacity(1);
    EXPECT_GT(fadeTarget, 0.0f);
    EXPECT_LT(fadeTarget, 1.0f);

    // 更靠近地平线 → 更透明(单调性)。
    LabelPlacement fresh;
    const Vec3 p2 = spherePoint(59.9);
    const auto in2 = makeInput(Vec3(2, 0, 0), p2);
    fresh.update(in2, {makeCandidate(1, p2)});
    ASSERT_EQ(1, fresh.stats().placed);
    EXPECT_LT(fresh.opacity(1), fadeTarget);
}

TEST_F(LabelPlacementTest, VanishedFeatureStateDropped) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    placement.update(in, {makeCandidate(1, spherePoint(0.0))});
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));

    // 要素删除(候选集为空):状态清扫,查询回 0。
    placement.update(in, {});
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(1));
}

TEST_F(LabelPlacementTest, DeterministicAcrossFrames) {
    // 同输入连续多帧:placed/collided 结果稳定(不闪)。
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const Vec3 p = spherePoint(0.0);
    const std::vector<LabelCandidate> cands = {makeCandidate(3, p),
                                               makeCandidate(7, p)};

    for (int i = 0; i < 5; ++i) {
        placement.update(in, cands);
        EXPECT_FLOAT_EQ(1.0f, placement.opacity(3));
        EXPECT_FLOAT_EQ(0.0f, placement.opacity(7));
    }
}
