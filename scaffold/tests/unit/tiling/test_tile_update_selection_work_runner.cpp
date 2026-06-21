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
