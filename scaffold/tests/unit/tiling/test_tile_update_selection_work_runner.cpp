#include <gtest/gtest.h>

#include "earth_engine/tiling/TileUpdateSelectionWorkRunner.h"

using namespace earth_engine;

TEST(
    TileUpdateSelectionWorkRunnerTest,
    ReuseRefreshesRenderEntriesAndPumpsQueuedRequests) {
    TilePlan tilePlan;
    TileLoadQueue loadQueue;
    const TileKey queuedKey{"test", 3, 4, 5};
    loadQueue.queue(queuedKey, TileLoadPriorityGroup::Normal, 1.0);
    TileSelectionCounters counters;
    TileSelectionReuseState reuseState;
    std::vector<ActivatedRasterOverlay*> overlays;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(42, config);
    FrameState frameState;
    frameState.frameId = 42;

    bool refreshCalled = false;
    bool selectCalled = false;
    bool requestCalled = false;
    size_t requestCount = 0;

    const TileUpdateSelectionWorkResult result =
        TileUpdateSelectionWorkRunner::run(
            TileUpdateSelectionWorkInput{
                tilePlan,
                loadQueue,
                counters,
                reuseState,
                overlays,
                budget,
                nullptr,
                frameState,
                1,
                1,
                TileSelectionReuseMode::Strict,
                TileSelectionReuseRejectReason::None,
                true,
                16.0},
            [&]() { refreshCalled = true; },
            [&](const FrameState&) { selectCalled = true; },
            [&](const TileKey&) -> TilesetTile* { return nullptr; },
            [](TilesetTile&) {},
            [&](const std::vector<TileLoadRequest>& requests,
                FrameResourceBudget*) {
                requestCalled = true;
                requestCount = requests.size();
                TileLoadRequestOutcome outcome;
                outcome.issued = 1;
                return outcome;
            });

    EXPECT_TRUE(result.reusedSelection);
    EXPECT_TRUE(refreshCalled);
    EXPECT_FALSE(selectCalled);
    ASSERT_EQ(loadQueue.size(), 1u);
    EXPECT_EQ(loadQueue.front().key, queuedKey);
    EXPECT_TRUE(requestCalled);
    EXPECT_EQ(requestCount, 1u);
    EXPECT_TRUE(reuseState.lastRequestIssuedWork);
}

TEST(
    TileUpdateSelectionWorkRunnerTest,
    ReselectCommitsReuseStateAndPumpsQueuedRequests) {
    TilePlan tilePlan;
    TileLoadQueue loadQueue;
    const TileKey queuedKey{"test", 2, 0, 1};
    TileSelectionCounters counters;
    counters.visited = 99;
    TileSelectionReuseState reuseState;
    std::vector<ActivatedRasterOverlay*> overlays;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(43, config);
    FrameState frameState;
    frameState.frameId = 43;
    frameState.viewportWidthPixels = 800;
    frameState.viewportHeightPixels = 600;

    bool refreshCalled = false;
    bool selectCalled = false;
    bool requestCalled = false;
    size_t requestCount = 0;

    const TileUpdateSelectionWorkResult result =
        TileUpdateSelectionWorkRunner::run(
            TileUpdateSelectionWorkInput{
                tilePlan,
                loadQueue,
                counters,
                reuseState,
                overlays,
                budget,
                nullptr,
                frameState,
                7,
                11,
                TileSelectionReuseMode::None,
                TileSelectionReuseRejectReason::SelectorMovedStaleDisabled,
                false,
                16.0},
            [&]() { refreshCalled = true; },
            [&](const FrameState& selectedFrame) {
                selectCalled = true;
                tilePlan.frameId = selectedFrame.frameId;
                loadQueue.queue(queuedKey, TileLoadPriorityGroup::Normal, 2.0);
            },
            [&](const TileKey&) -> TilesetTile* { return nullptr; },
            [](TilesetTile&) {},
            [&](const std::vector<TileLoadRequest>& requests,
                FrameResourceBudget*) {
                requestCalled = true;
                requestCount = requests.size();
                TileLoadRequestOutcome outcome;
                outcome.blockedByInflight = true;
                return outcome;
            });

    EXPECT_FALSE(result.reusedSelection);
    EXPECT_EQ(result.reuseMode, TileSelectionReuseMode::None);
    EXPECT_EQ(
        result.reuseRejectReason,
        TileSelectionReuseRejectReason::SelectorMovedStaleDisabled);
    EXPECT_TRUE(selectCalled);
    EXPECT_FALSE(refreshCalled);
    EXPECT_EQ(tilePlan.frameId, frameState.frameId);
    ASSERT_EQ(loadQueue.size(), 1u);
    EXPECT_EQ(loadQueue.front().key, queuedKey);
    EXPECT_TRUE(requestCalled);
    EXPECT_EQ(requestCount, 1u);
    EXPECT_TRUE(reuseState.lastRequestBlockedByInflight);

    FrameState nextFrame = frameState;
    nextFrame.frameId = 44;
    EXPECT_EQ(
        reuseState.classifyReuse(
            nextFrame,
            7,
            11,
            false,
            false,
            false,
            false),
        TileSelectionReuseMode::Strict);
}
