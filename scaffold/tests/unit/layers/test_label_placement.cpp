#include <gtest/gtest.h>

#include "earth_engine/layers/LabelPlacement.h"
#include "earth_engine/scene/Camera.h"
#include "earth_engine/style/AmapClassicLabelStyleInternal.h"

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

TEST(LabelPlacementContractTest, ReadableDirectionFlipsLeftwardTangent) {
    const auto d = LabelPlacement::readableScreenDirection(-3.0, 4.0);
    EXPECT_NEAR(0.6, d[0], 1e-9);
    EXPECT_NEAR(-0.8, d[1], 1e-9);
    const auto horizontal = LabelPlacement::readableScreenDirection(0.0, 0.0);
    EXPECT_DOUBLE_EQ(1.0, horizontal[0]);
    EXPECT_DOUBLE_EQ(0.0, horizontal[1]);
}

TEST(LabelPlacementContractTest, RotatedCollisionBoundsFollowLineDirection) {
    const auto horizontal = LabelPlacement::rotatedScreenBounds(
        -40.0f, -5.0f, 40.0f, 5.0f, 1.0, 0.0);
    const auto vertical = LabelPlacement::rotatedScreenBounds(
        -40.0f, -5.0f, 40.0f, 5.0f, 0.0, 1.0);
    EXPECT_NEAR(80.0, horizontal[2] - horizontal[0], 1e-9);
    EXPECT_NEAR(10.0, horizontal[3] - horizontal[1], 1e-9);
    EXPECT_NEAR(10.0, vertical[2] - vertical[0], 1e-9);
    EXPECT_NEAR(80.0, vertical[3] - vertical[1], 1e-9);
}

TEST_F(LabelPlacementTest, CandidateSpecificPaddingControlsCollision) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const Vec3 anchor = spherePoint(0.0);
    LabelCandidate left = makeCandidate(1, anchor);
    LabelCandidate right = makeCandidate(2, anchor);
    left.boxMinXPx = -20.0f;
    left.boxMaxXPx = -1.0f;
    right.boxMinXPx = 1.0f;
    right.boxMaxXPx = 20.0f;
    left.paddingXPx = right.paddingXPx = 0.0f;
    left.paddingYPx = right.paddingYPx = 0.0f;
    placement.update(in, {left, right});
    EXPECT_EQ(2, placement.stats().placed);

    LabelPlacement padded;
    left.paddingXPx = right.paddingXPx = 2.0f;
    left.paddingYPx = right.paddingYPx = 2.0f;
    padded.update(in, {left, right});
    EXPECT_EQ(1, padded.stats().placed);
    EXPECT_EQ(1, padded.stats().collided);
}

TEST_F(LabelPlacementTest, CandidatePaddingIsAxisSpecific) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const Vec3 anchor = spherePoint(0.0);
    LabelCandidate left = makeCandidate(31, anchor);
    LabelCandidate right = makeCandidate(32, anchor);
    left.boxMinXPx = -20.0f;
    left.boxMaxXPx = -1.0f;
    right.boxMinXPx = 1.0f;
    right.boxMaxXPx = 20.0f;
    left.paddingXPx = right.paddingXPx = 2.0f;
    left.paddingYPx = right.paddingYPx = 0.0f;
    placement.update(in, {left, right});
    EXPECT_EQ(1, placement.stats().collided);

    LabelPlacement verticalOnly;
    left.paddingXPx = right.paddingXPx = 0.0f;
    left.paddingYPx = right.paddingYPx = 2.0f;
    verticalOnly.update(in, {left, right});
    EXPECT_EQ(0, verticalOnly.stats().collided);
}

TEST_F(LabelPlacementTest, SecondaryIconBoxCollidesWithoutBlockingGap) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const Vec3 p = spherePoint(0.0);
    LabelCandidate withIcon = makeCandidate(1, p);
    withIcon.boxMinXPx = 20.0f;
    withIcon.boxMaxXPx = 40.0f;
    withIcon.boxMinYPx = -5.0f;
    withIcon.boxMaxYPx = 5.0f;
    withIcon.paddingXPx = withIcon.paddingYPx = 0.0f;
    withIcon.hasSecondaryBox = true;
    withIcon.secondaryBoxMinXPx = -40.0f;
    withIcon.secondaryBoxMaxXPx = -20.0f;
    withIcon.secondaryBoxMinYPx = -5.0f;
    withIcon.secondaryBoxMaxYPx = 5.0f;

    LabelCandidate hitsIcon = makeCandidate(2, p);
    hitsIcon.boxMinXPx = -35.0f;
    hitsIcon.boxMaxXPx = -25.0f;
    hitsIcon.boxMinYPx = -4.0f;
    hitsIcon.boxMaxYPx = 4.0f;
    hitsIcon.paddingXPx = hitsIcon.paddingYPx = 0.0f;
    placement.update(in, {withIcon, hitsIcon});
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(2));

    LabelPlacement gapPlacement;
    LabelCandidate inGap = hitsIcon;
    inGap.featureId = 3;
    inGap.boxMinXPx = -5.0f;
    inGap.boxMaxXPx = 5.0f;
    gapPlacement.update(in, {withIcon, inGap});
    EXPECT_FLOAT_EQ(1.0f, gapPlacement.opacity(1));
    EXPECT_FLOAT_EQ(1.0f, gapPlacement.opacity(3));
}

TEST_F(LabelPlacementTest,
       AlongPathCollisionPartsReplaceCenteredBoundingRectangle) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const Vec3 p = spherePoint(0.0);
    LabelCandidate curved = makeCandidate(1, p, 80.0f, 40.0f);
    curved.paddingXPx = curved.paddingYPx = 0.0f;
    curved.collisionParts = {
        LabelCollisionPart{p, p, -80.0f, -8.0f, -50.0f, 8.0f},
        LabelCollisionPart{p, p, 50.0f, -8.0f, 80.0f, 8.0f}};

    LabelCandidate inRealGap = makeCandidate(2, p, 10.0f, 6.0f);
    inRealGap.paddingXPx = inRealGap.paddingYPx = 0.0f;
    placement.update(in, {curved, inRealGap});
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(2));

    LabelPlacement hitPlacement;
    LabelCandidate hitsGlyph = makeCandidate(3, p, 10.0f, 6.0f);
    hitsGlyph.boxMinXPx = -70.0f;
    hitsGlyph.boxMaxXPx = -55.0f;
    hitsGlyph.paddingXPx = hitsGlyph.paddingYPx = 0.0f;
    hitPlacement.update(in, {curved, hitsGlyph});
    EXPECT_FLOAT_EQ(1.0f, hitPlacement.opacity(1));
    EXPECT_FLOAT_EQ(0.0f, hitPlacement.opacity(3));
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

TEST_F(LabelPlacementTest,
       OfficialEqualRankUsesExplicitStampInsteadOfVectorDistanceOrId) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    LabelCandidate earlier = makeCandidate(
        1, spherePoint(20.0), 400.0f, 300.0f, 4);
    LabelCandidate later = makeCandidate(
        999, spherePoint(0.0), 400.0f, 300.0f, 4);
    // Deliberately put the larger official stamp first in the candidate
    // vector. Vector position, camera distance, and feature id all disagree
    // with the provider contract; only the explicit stamp may decide.
    earlier.officialInsertionOrder = 42;
    later.officialInsertionOrder = 41;

    placement.update(in, {earlier, later});
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(999));
}

TEST_F(LabelPlacementTest,
       OfficialLaterPathFragmentWinsWithinOneSourceStamp) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    LabelCandidate first = makeCandidate(1001, spherePoint(0.0));
    LabelCandidate second = makeCandidate(1002, spherePoint(0.0));
    first.officialInsertionOrder = second.officialInsertionOrder = 42;
    first.officialFragmentOrder = 0;
    second.officialFragmentOrder = 1;

    placement.update(in, {first, second});
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(1001));
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1002));
}

TEST_F(LabelPlacementTest,
       OfficialCanCoveredYieldsOnlyToHigherPriorityAndBlocksNobody) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const Vec3 p = spherePoint(0.0);
    LabelCandidate higherCanCovered = makeCandidate(1, p);
    higherCanCovered.rank = 1;
    higherCanCovered.officialCanCovered =
        amapClassicPoiCanCovered(10002, 32, 4.0);
    ASSERT_TRUE(higherCanCovered.officialCanCovered);
    LabelCandidate lowerOrdinary = makeCandidate(2, p);
    lowerOrdinary.rank = 2;
    placement.update(in, {higherCanCovered, lowerOrdinary});
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(2));

    LabelPlacement higherOrdinaryWins;
    LabelCandidate higherOrdinary = makeCandidate(3, p);
    higherOrdinary.rank = 1;
    LabelCandidate lowerCanCovered = makeCandidate(4, p);
    lowerCanCovered.rank = 2;
    lowerCanCovered.officialCanCovered =
        amapClassicPoiCanCovered(10002, 34, 6.0);
    ASSERT_TRUE(lowerCanCovered.officialCanCovered);
    higherOrdinaryWins.update(in, {higherOrdinary, lowerCanCovered});
    EXPECT_FLOAT_EQ(1.0f, higherOrdinaryWins.opacity(3));
    EXPECT_FLOAT_EQ(0.0f, higherOrdinaryWins.opacity(4));
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

TEST_F(LabelPlacementTest, SameLineRepeatGroupHonorsMinimumScreenDistance) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    LabelCandidate a = makeCandidate(101, spherePoint(-1.0), 4.0f, 4.0f);
    LabelCandidate b = makeCandidate(102, spherePoint(1.0), 4.0f, 4.0f);
    a.repeatGroup = 77;
    b.repeatGroup = 77;
    a.repeatDistancePx = 200.0f;
    b.repeatDistancePx = 200.0f;

    placement.update(in, {a, b});
    EXPECT_EQ(1, placement.stats().placed);
    EXPECT_EQ(1, placement.stats().repeated);
    EXPECT_EQ(0, placement.stats().collided);
}

TEST_F(LabelPlacementTest, DifferentRepeatGroupsRemainIndependent) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    LabelCandidate a = makeCandidate(201, spherePoint(-2.0), 2.0f, 2.0f);
    LabelCandidate b = makeCandidate(202, spherePoint(2.0), 2.0f, 2.0f);
    a.repeatGroup = 11;
    b.repeatGroup = 12;
    a.repeatDistancePx = 500.0f;
    b.repeatDistancePx = 500.0f;

    placement.update(in, {a, b});
    EXPECT_EQ(2, placement.stats().placed);
    EXPECT_EQ(0, placement.stats().repeated);
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

// 热点③ 视锥预剔除:锚点投到屏外(超出盒外接半径余量)时,整盒必在屏外,
// 在昂贵计算(tangent/沿线部件投影/旋转包围盒)之前即被剔除,结果与先算
// 完再剔等价 —— placed/collided 不变,只省白算。锚点 (0,3,0) 在相机
// (2,0,0) 前方、高横向角投到屏右外侧(单位球面点水平角上限 30° < 半视场,
// 故须用超球面半径锚点),小盒 maxR 小 → 应预剔除。
// 热点③ boxFullyOffscreenScreen:屏外保守剔除助手(collect/update 共用)。
// 锚点投影 + 盒外接半径:屏外返回 true,屏上/跨屏返回 false。相机背后 true。
TEST_F(LabelPlacementTest, BoxFullyOffscreenHelperCullsAndPreserves) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const auto& vp = in.viewProj;
    const double vpW = in.viewportWidthPx, vpH = in.viewportHeightPx;

    // 屏中央小盒 → 不剔除。
    EXPECT_FALSE(LabelPlacement::boxFullyOffscreenScreen(
        spherePoint(0.0), vp, vpW, vpH, -20, -10, 20, 10, false, 0, 0, 0, 0,
        0, 0));

    // 屏右外侧小盒 → 剔除。
    EXPECT_TRUE(LabelPlacement::boxFullyOffscreenScreen(
        Vec3(0.0, 3.0, 0.0), vp, vpW, vpH, -4, -4, 4, 4, false, 0, 0, 0, 0,
        0, 0));

    // 锚点屏外但盒跨入屏内(大左盒)→ 不剔除。
    EXPECT_FALSE(LabelPlacement::boxFullyOffscreenScreen(
        Vec3(0.6, 1.5, 0.0), vp, vpW, vpH, -500, -15, 300, 15, false, 0, 0,
        0, 0, 0, 0));

    // 相机背后 → 剔除。
    EXPECT_TRUE(LabelPlacement::boxFullyOffscreenScreen(
        Vec3(3.0, 0.0, 0.0), vp, vpW, vpH, -20, -10, 20, 10, false, 0, 0, 0,
        0, 0, 0));

    // secondary 盒跨入屏内 → 主盒屏外也不剔除。
    EXPECT_FALSE(LabelPlacement::boxFullyOffscreenScreen(
        Vec3(0.0, 3.0, 0.0), vp, vpW, vpH, -4, -4, 4, 4, true, -900, -10, 300,
        10, 0, 0));
}

TEST_F(LabelPlacementTest, FullyOffscreenAnchorPreCulled) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    const std::vector<LabelCandidate> cands = {
        makeCandidate(1, Vec3(0.0, 3.0, 0.0))};

    placement.update(in, cands);
    EXPECT_EQ(1, placement.stats().culledProjection);
    EXPECT_EQ(0, placement.stats().placed);
    EXPECT_FLOAT_EQ(0.0f, placement.opacity(1));
}

// 预剔除不得误伤"锚点屏外但盒跨入屏内"的渐进平移标签:锚点 (0.6,1.5,0)
// 高横向角投到屏右外侧(px>0.5 避开 horizon 剔除),盒向屏内延伸跨入视口
// → 预剔除(整盒必屏外)判定不应触发,应 placed。这钉死 pre-cull 只用"外接
// 圆半径"界定、不误杀盒跨入视口的标签。
TEST_F(LabelPlacementTest, AnchorOffscreenWithBoxCrossingInSurvivesPreCull) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    auto c = makeCandidate(1, Vec3(0.6, 1.5, 0.0), 400.0f, 15.0f);
    c.boxMinXPx = -500.0f;
    c.boxMaxXPx = 300.0f;
    placement.update(in, {c});
    EXPECT_EQ(1, placement.stats().placed);
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));
}

TEST_F(LabelPlacementTest, PartiallyOffscreenLabelRemainsPlaced) {
    const auto in = makeInput(Vec3(2, 0, 0), Vec3(0, 0, 0));
    // The point is visible, but the label crosses the left edge. Continuous
    // map panning keeps the label alive until its complete box leaves.
    auto candidate = makeCandidate(1, spherePoint(0.0), 400.0f, 15.0f);
    candidate.boxMinXPx = -500.0f;
    candidate.boxMaxXPx = 300.0f;

    placement.update(in, {candidate});
    EXPECT_EQ(1, placement.stats().placed);
    EXPECT_FLOAT_EQ(1.0f, placement.opacity(1));
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
