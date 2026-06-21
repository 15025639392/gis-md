#include <gtest/gtest.h>

#include "earth_engine/scene/Camera.h"
#include "earth_engine/tiling/TileFrameWorkCoordinator.h"

using namespace earth_engine;

TEST(
    TileFrameWorkCoordinatorTest,
    ActiveInteractionReselectsInsteadOfStaleReuse) {
    FrameState previousFrame;
    previousFrame.frameId = 10;
    previousFrame.viewportWidthPixels = 800;
    previousFrame.viewportHeightPixels = 600;
    SelectorView previousView;
    previousView.position = Vec3(1000.0, 0.0, 0.0);
    previousView.direction = Vec3(0.0, 1.0, 0.0);
    previousView.viewportHeightPixels = 600;
    previousFrame.selectorViews.push_back(previousView);

    std::vector<ActivatedRasterOverlay*> overlays;
    TileSelectionReuseState reuseState;
    reuseState.commit(
        previousFrame,
        1,
        TileRasterOverlaySignature::configuration(overlays));

    Camera camera;
    camera.setView(
        Vec3(1050.0, 0.0, 0.0),
        Vec3(0.005, 0.9999875, 0.0),
        Vec3(0.0, 0.0, 1.0));

    FrameState movingFrame = previousFrame;
    movingFrame.frameId = 11;
    movingFrame.timeSeconds = 1.0;
    movingFrame.camera = &camera;
    movingFrame.selectorViews[0].position = Vec3(1050.0, 0.0, 0.0);
    movingFrame.selectorViews[0].direction = Vec3(0.005, 0.9999875, 0.0);

    TilePlan tilePlan;
    TileLoadQueue loadQueue;
    TileSelectionCounters counters;
    FrameResourceBudget budget;
    Vec3 lastCameraPosition = Vec3(1000.0, 0.0, 0.0);
    Vec3 lastCameraDirection = Vec3(0.0, 1.0, 0.0);
    bool cameraMoving = false;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
    double lastInteractionActiveTimeSeconds = -1.0;

    bool refreshCalled = false;
    bool selectCalled = false;

    const TileFrameWorkResult result = TileFrameWorkCoordinator::run(
        TileFrameWorkInput{
            tilePlan,
            loadQueue,
            counters,
            reuseState,
            overlays,
            budget,
            nullptr,
            movingFrame,
            1,
            20,
            20,
            0.0,
            1.25,
            16.0},
        TileFrameWorkState{
            cameraMoving,
            interactionActive,
            resourceSmoothingActive,
            lastInteractionActiveTimeSeconds,
            lastCameraPosition,
            lastCameraDirection},
        [](bool, bool, FrameResourceBudget*) { return false; },
        []() {},
        []() { return true; },
        [&]() { refreshCalled = true; },
        [&](const FrameState&) { selectCalled = true; },
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [](const std::vector<TileLoadRequest>&, FrameResourceBudget*) {
            return TileLoadRequestOutcome{};
        });

    EXPECT_TRUE(result.interactionActive);
    EXPECT_TRUE(result.resourceSmoothingActive);
    EXPECT_TRUE(selectCalled);
    EXPECT_FALSE(refreshCalled);
    EXPECT_EQ(result.selectionWork.reuseMode, TileSelectionReuseMode::None);
    EXPECT_EQ(
        result.selectionWork.reuseRejectReason,
        TileSelectionReuseRejectReason::SelectorMovedStaleDisabled);
}

TEST(TileFrameWorkCoordinatorTest, StableFrameStrictReusesSelection) {
    FrameState previousFrame;
    previousFrame.frameId = 10;
    previousFrame.viewportWidthPixels = 800;
    previousFrame.viewportHeightPixels = 600;
    SelectorView previousView;
    previousView.position = Vec3(1000.0, 0.0, 0.0);
    previousView.direction = Vec3(0.0, 1.0, 0.0);
    previousView.viewportHeightPixels = 600;
    previousFrame.selectorViews.push_back(previousView);

    std::vector<ActivatedRasterOverlay*> overlays;
    TileSelectionReuseState reuseState;
    reuseState.commit(
        previousFrame,
        1,
        TileRasterOverlaySignature::configuration(overlays));

    FrameState stableFrame = previousFrame;
    stableFrame.frameId = 11;
    stableFrame.timeSeconds = 1.0;
    Camera camera;
    camera.setView(
        previousView.position,
        previousView.direction,
        Vec3(0.0, 0.0, 1.0));
    stableFrame.camera = &camera;

    TilePlan tilePlan;
    tilePlan.frameId = previousFrame.frameId;
    TileLoadQueue loadQueue;
    const TileKey queuedKey{"test", 1, 0, 0};
    loadQueue.queue(queuedKey, TileLoadPriorityGroup::Normal, 1.0);
    TileSelectionCounters counters;
    FrameResourceBudget budget;
    Vec3 lastCameraPosition = previousView.position;
    Vec3 lastCameraDirection = previousView.direction;
    bool cameraMoving = true;
    bool interactionActive = true;
    bool resourceSmoothingActive = true;
    double lastInteractionActiveTimeSeconds = -1.0;

    bool refreshCalled = false;
    bool selectCalled = false;
    bool requestCalled = false;

    const TileFrameWorkResult result = TileFrameWorkCoordinator::run(
        TileFrameWorkInput{
            tilePlan,
            loadQueue,
            counters,
            reuseState,
            overlays,
            budget,
            nullptr,
            stableFrame,
            1,
            20,
            20,
            0.0,
            1.25,
            16.0},
        TileFrameWorkState{
            cameraMoving,
            interactionActive,
            resourceSmoothingActive,
            lastInteractionActiveTimeSeconds,
            lastCameraPosition,
            lastCameraDirection},
        [](bool, bool, FrameResourceBudget*) { return false; },
        []() {},
        []() { return false; },
        [&]() { refreshCalled = true; },
        [&](const FrameState&) { selectCalled = true; },
        [](const TileKey&) -> TilesetTile* { return nullptr; },
        [&requestCalled](
            const std::vector<TileLoadRequest>& requests,
            FrameResourceBudget*) {
            requestCalled = true;
            TileLoadRequestOutcome outcome;
            outcome.issued = requests.empty() ? 0 : 1;
            return outcome;
        });

    EXPECT_FALSE(result.interactionActive);
    EXPECT_FALSE(result.resourceSmoothingActive);
    EXPECT_FALSE(cameraMoving);
    EXPECT_FALSE(interactionActive);
    EXPECT_FALSE(resourceSmoothingActive);
    EXPECT_TRUE(refreshCalled);
    EXPECT_FALSE(selectCalled);
    EXPECT_TRUE(requestCalled);
    EXPECT_TRUE(result.selectionWork.reusedSelection);
    EXPECT_EQ(result.selectionWork.reuseMode, TileSelectionReuseMode::Strict);
    EXPECT_EQ(
        result.selectionWork.reuseRejectReason,
        TileSelectionReuseRejectReason::None);
    EXPECT_TRUE(reuseState.lastRequestIssuedWork);
}
