#include <gtest/gtest.h>

#include "earth_engine/tiling/TileSelectionReusePolicy.h"
#include "earth_engine/tiling/TileSelectionReuseState.h"

using namespace earth_engine;

namespace {

FrameState makeBaseFrame(uint64_t frameId) {
    FrameState frame;
    frame.frameId = frameId;
    frame.viewportWidthPixels = 800;
    frame.viewportHeightPixels = 600;
    SelectorView view;
    view.position = Vec3(1000.0, 0.0, 0.0);
    view.direction = Vec3(0.0, 1.0, 0.0);
    view.viewportHeightPixels = 600;
    frame.selectorViews.push_back(view);
    return frame;
}

FrameState makeMovingFrame(const FrameState& previousFrame,
                           uint64_t frameId) {
    FrameState frame = previousFrame;
    frame.frameId = frameId;
    frame.selectorViews[0].position = Vec3(1050.0, 0.0, 0.0);
    frame.selectorViews[0].direction = Vec3(0.005, 0.9999875, 0.0);
    return frame;
}

TileSelectionReuseInput makeInput(
    const FrameState& frameState,
    const FrameState& previousFrame,
    bool allowStaleSelection) {
    return TileSelectionReuseInput{
        frameState,
        previousFrame.selectorViews,
        7,
        7,
        9,
        9,
        previousFrame.viewportWidthPixels,
        previousFrame.viewportHeightPixels,
        frameState.frameId,
        previousFrame.frameId,
        1,
        100.0,
        1e-4,
        true,
        allowStaleSelection,
        true,
        true,
        true,
        true};
}

} // namespace

TEST(TileSelectionReusePolicyTest, EquivalentViewsClassifyAsStrictReuse) {
    const FrameState previousFrame = makeBaseFrame(10);
    FrameState stillFrame = previousFrame;
    stillFrame.frameId = previousFrame.frameId + 1;

    TileSelectionReuseInput input =
        makeInput(stillFrame, previousFrame, false);
    input.hasPendingTilesetWork = false;
    input.hasPendingRasterOverlayWork = false;
    input.lastRequestIssuedWork = false;
    input.lastRequestBlockedByInflight = false;

    EXPECT_TRUE(TileSelectionReusePolicy::canReuseSelection(input));
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuse(input),
        TileSelectionReuseMode::Strict);
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuseWithReason(input).rejectReason,
        TileSelectionReuseRejectReason::None);
}

TEST(
    TileSelectionReusePolicyTest,
    EquivalentViewsReuseWhilePendingWorkDrains) {
    const FrameState previousFrame = makeBaseFrame(10);
    FrameState stillFrame = previousFrame;
    stillFrame.frameId = previousFrame.frameId + 1;

    const TileSelectionReuseInput input =
        makeInput(stillFrame, previousFrame, false);

    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuse(input),
        TileSelectionReuseMode::Strict);
}

TEST(
    TileSelectionReusePolicyTest,
    MovedViewsRejectWhenStaleReuseDisabled) {
    const FrameState previousFrame = makeBaseFrame(10);
    const FrameState movingFrame = makeMovingFrame(previousFrame, 11);

    const TileSelectionReuseInput input =
        makeInput(movingFrame, previousFrame, false);

    EXPECT_FALSE(TileSelectionReusePolicy::canReuseSelection(input));
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuse(input),
        TileSelectionReuseMode::None);
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuseWithReason(input).rejectReason,
        TileSelectionReuseRejectReason::SelectorMovedStaleDisabled);
}

TEST(
    TileSelectionReusePolicyTest,
    SmallMovedViewsClassifyAsStaleWhenEnabled) {
    const FrameState previousFrame = makeBaseFrame(10);
    const FrameState movingFrame = makeMovingFrame(previousFrame, 11);

    const TileSelectionReuseInput input =
        makeInput(movingFrame, previousFrame, true);

    EXPECT_TRUE(TileSelectionReusePolicy::canReuseSelection(input));
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuse(input),
        TileSelectionReuseMode::Stale);
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuseWithReason(input).rejectReason,
        TileSelectionReuseRejectReason::None);
}

TEST(TileSelectionReusePolicyTest, ResourceRevisionChangeRejectsReuse) {
    const FrameState previousFrame = makeBaseFrame(10);
    FrameState stillFrame = previousFrame;
    stillFrame.frameId = previousFrame.frameId + 1;

    TileSelectionReuseInput input =
        makeInput(stillFrame, previousFrame, false);
    input.currentResourceRevision = 8;

    EXPECT_FALSE(TileSelectionReusePolicy::canReuseSelection(input));
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuse(input),
        TileSelectionReuseMode::None);
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuseWithReason(input).rejectReason,
        TileSelectionReuseRejectReason::ResourceChanged);
}

TEST(TileSelectionReusePolicyTest, OverlaySignatureChangeRejectsReuse) {
    const FrameState previousFrame = makeBaseFrame(10);
    FrameState stillFrame = previousFrame;
    stillFrame.frameId = previousFrame.frameId + 1;

    TileSelectionReuseInput input =
        makeInput(stillFrame, previousFrame, false);
    input.currentOverlaySignature = 10;

    EXPECT_FALSE(TileSelectionReusePolicy::canReuseSelection(input));
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuseWithReason(input).rejectReason,
        TileSelectionReuseRejectReason::ResourceChanged);
}

TEST(TileSelectionReusePolicyTest, ViewportChangeRejectsReuse) {
    const FrameState previousFrame = makeBaseFrame(10);
    FrameState resizedFrame = previousFrame;
    resizedFrame.frameId = previousFrame.frameId + 1;
    resizedFrame.viewportWidthPixels = 1024;

    const TileSelectionReuseInput input =
        makeInput(resizedFrame, previousFrame, false);

    EXPECT_FALSE(TileSelectionReusePolicy::canReuseSelection(input));
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuseWithReason(input).rejectReason,
        TileSelectionReuseRejectReason::ViewportChanged);
}

TEST(TileSelectionReusePolicyTest, StaleAgeWindowRejectsOlderReuse) {
    const FrameState previousFrame = makeBaseFrame(10);
    const FrameState movingFrame = makeMovingFrame(previousFrame, 12);

    TileSelectionReuseInput input =
        makeInput(movingFrame, previousFrame, true);
    input.maxStaleFrameAge = 1;

    EXPECT_FALSE(TileSelectionReusePolicy::canReuseSelection(input));
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuseWithReason(input).rejectReason,
        TileSelectionReuseRejectReason::StaleAgeExceeded);
}

TEST(TileSelectionReusePolicyTest, StaleReuseRejectsLargeViewMovement) {
    const FrameState previousFrame = makeBaseFrame(10);
    FrameState farFrame = makeMovingFrame(previousFrame, 11);
    farFrame.selectorViews[0].position = Vec3(1200.0, 0.0, 0.0);

    const TileSelectionReuseInput input =
        makeInput(farFrame, previousFrame, true);

    EXPECT_FALSE(TileSelectionReusePolicy::canReuseSelection(input));
    EXPECT_EQ(
        TileSelectionReusePolicy::classifyReuseWithReason(input).rejectReason,
        TileSelectionReuseRejectReason::StaleViewTooDifferent);
}

TEST(TileSelectionReuseStateTest, DefaultStaleWindowSpansThreeFrames) {
    const FrameState previousFrame = makeBaseFrame(10);
    TileSelectionReuseState reuseState;
    reuseState.commit(previousFrame, 7, 9);
    reuseState.recordRequestOutcome(true, true);

    const FrameState thirdStaleFrame = makeMovingFrame(previousFrame, 13);
    EXPECT_EQ(
        reuseState.classifyReuse(
            thirdStaleFrame,
            7,
            9,
            true,
            true,
            true),
        TileSelectionReuseMode::Stale);

    const FrameState fourthStaleFrame = makeMovingFrame(previousFrame, 14);
    EXPECT_EQ(
        reuseState.classifyReuse(
            fourthStaleFrame,
            7,
            9,
            true,
            true,
            true),
        TileSelectionReuseMode::None);
}
