#include <gtest/gtest.h>

#include "earth_engine/tiling/TileUpdateSelectionWorkRunner.h"
#include "earth_engine/tiling/TileContentCacheManager.h"
#include "earth_engine/tiling/TileContentResourceInvalidator.h"
#include "earth_engine/tiling/TilesetTile.h"

#include "../../helpers/MockRenderDevice.h"

#include <memory>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

std::string testCacheKeyFor(const TileKey& key) {
    return key.schemeId.str() + ":" + std::to_string(key.z) + ":" +
           std::to_string(key.x) + ":" + std::to_string(key.y);
}

} // namespace

TEST(
    TileUpdateSelectionWorkRunnerTest,
    StrictReusePreservesResolvedRenderPlanAndConsumesIssuedRequests) {
    TilePlan tilePlan;
    tilePlan.frameId = 41;
    tilePlan.frameCredits = {"existing-credit"};
    tilePlan.frameProgressTotalCount = 7;
    tilePlan.frameProgressLoadingCount = 2;
    tilePlan.frameLoadProgressPercentage = 71.0;
    TileRenderEntry existingEntry;
    existingEntry.selectedKey = TileKey{"test", 2, 1, 1};
    existingEntry.renderKey = existingEntry.selectedKey;
    tilePlan.renderEntries.push_back(existingEntry);
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
            [&](const FrameState&, TileSelectionPerformanceTimings&) {
                selectCalled = true;
            },
            [](const TileKey&) -> TilesetTile* { return nullptr; },
            [&](const TileKey&) -> TilesetTile* { return nullptr; },
            [](TilesetTile&) {},
            [](TilesetTile&) {},
            [&](TileLoadQueue& requests,
                FrameResourceBudget*) {
                requestCalled = true;
                requestCount = requests.size();
                requests.clear();
                TileLoadRequestOutcome outcome;
                outcome.issued = 1;
                return outcome;
            },
            [](TilesetTile&) {});

    EXPECT_TRUE(result.reusedSelection);
    EXPECT_FALSE(refreshCalled);
    EXPECT_FALSE(selectCalled);
    EXPECT_EQ(42u, tilePlan.frameId);
    ASSERT_EQ(1u, tilePlan.renderEntries.size());
    EXPECT_EQ(existingEntry.selectedKey,
              tilePlan.renderEntries.front().selectedKey);
    EXPECT_EQ(std::vector<std::string>({"existing-credit"}),
              tilePlan.frameCredits);
    EXPECT_EQ(7, tilePlan.frameProgressTotalCount);
    EXPECT_EQ(2, tilePlan.frameProgressLoadingCount);
    EXPECT_DOUBLE_EQ(71.0, tilePlan.frameLoadProgressPercentage);
    EXPECT_TRUE(loadQueue.empty());
    EXPECT_TRUE(requestCalled);
    EXPECT_EQ(requestCount, 1u);
    EXPECT_TRUE(reuseState.lastRequestIssuedWork);
}

TEST(
    TileUpdateSelectionWorkRunnerTest,
    StaleReuseRefreshesRenderFacingState) {
    TilePlan tilePlan;
    TileLoadQueue loadQueue;
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
                2,
                1,
                TileSelectionReuseMode::Stale,
                TileSelectionReuseRejectReason::None,
                true,
                16.0},
            [&]() { refreshCalled = true; },
            [&](const FrameState&, TileSelectionPerformanceTimings&) {
                selectCalled = true;
            },
            [](const TileKey&) -> TilesetTile* { return nullptr; },
            [](const TileKey&) -> TilesetTile* { return nullptr; },
            [](TilesetTile&) {},
            [](TilesetTile&) {},
            [](TileLoadQueue&, FrameResourceBudget*) {
                return TileLoadRequestOutcome{};
            },
            [](TilesetTile&) {});

    EXPECT_TRUE(result.reusedSelection);
    EXPECT_EQ(TileSelectionReuseMode::Stale, result.reuseMode);
    EXPECT_TRUE(refreshCalled);
    EXPECT_FALSE(selectCalled);
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
            [&](const FrameState& selectedFrame,
                TileSelectionPerformanceTimings& timings) {
                selectCalled = true;
                timings.collectDetailed = true;
                timings.traversalMs = 8.0;
                timings.refineMs = 3.0;
                timings.renderPlanMs = 2.0;
                timings.refineOverlayMs = 0.5;
                timings.refineDecisionMs = 0.75;
                timings.refineMaterializeMs = 0.25;
                timings.refineCommitMs = 1.5;
                timings.refineMaterializeCalls = 10;
                timings.refineMaterializeChanged = 2;
                timings.refineMaterializeRetry = 1;
                timings.refineMaterializeFastPath = 7;
                tilePlan.frameId = selectedFrame.frameId;
                loadQueue.queue(queuedKey, TileLoadPriorityGroup::Normal, 2.0);
            },
            [](const TileKey&) -> TilesetTile* { return nullptr; },
            [&](const TileKey&) -> TilesetTile* { return nullptr; },
            [](TilesetTile&) {},
            [](TilesetTile&) {},
            [&](TileLoadQueue& requests,
                FrameResourceBudget*) {
                requestCalled = true;
                requestCount = requests.size();
                TileLoadRequestOutcome outcome;
                outcome.blockedByInflight = true;
                return outcome;
            },
            [](TilesetTile&) {});

    EXPECT_FALSE(result.reusedSelection);
    EXPECT_EQ(result.reuseMode, TileSelectionReuseMode::None);
    EXPECT_EQ(
        result.reuseRejectReason,
        TileSelectionReuseRejectReason::SelectorMovedStaleDisabled);
    EXPECT_TRUE(selectCalled);
    EXPECT_TRUE(result.selectorDetailedTimings);
    EXPECT_DOUBLE_EQ(result.selectorTraversalMs, 8.0);
    EXPECT_DOUBLE_EQ(result.selectorRefineMs, 3.0);
    EXPECT_DOUBLE_EQ(result.selectorRenderPlanMs, 2.0);
    EXPECT_DOUBLE_EQ(result.selectorRefineOverlayMs, 0.5);
    EXPECT_DOUBLE_EQ(result.selectorRefineDecisionMs, 0.75);
    EXPECT_DOUBLE_EQ(result.selectorRefineMaterializeMs, 0.25);
    EXPECT_DOUBLE_EQ(result.selectorRefineCommitMs, 1.5);
    EXPECT_EQ(result.selectorRefineMaterializeCalls, 10);
    EXPECT_EQ(result.selectorRefineMaterializeChanged, 2);
    EXPECT_EQ(result.selectorRefineMaterializeRetry, 1);
    EXPECT_EQ(result.selectorRefineMaterializeFastPath, 7);
    EXPECT_GE(result.selectorRequestPlanningMs, 0.0);
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
            false),
        TileSelectionReuseMode::Strict);
}

TEST(
    TileUpdateSelectionWorkRunnerTest,
    ReuseStillBuildsVisibleTerrainFillProxy) {
    TilePlan tilePlan;
    const TileKey key{"Geographic-TMS", 1, 0, 0};
    tilePlan.visibleTiles.push_back(key);
    TileLoadQueue loadQueue;
    TileSelectionCounters counters;
    TileSelectionReuseState reuseState;
    std::vector<ActivatedRasterOverlay*> overlays;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(44, config);
    FrameState frameState;
    frameState.frameId = 44;
    earth_engine::testing::MockRenderDevice device;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    auto tile = std::make_unique<TilesetTile>(
        key,
        Rectangle::fromDegrees(-1.0, -1.0, 1.0, 1.0));
    TilesetTile* tilePtr = tile.get();
    tiles.emplace(testCacheKeyFor(key), std::move(tile));

    int refreshCallCount = 0;
    bool refreshSawReadyFill = false;
    bool selectCalled = false;
    bool tileResourcesChanged = false;
    int findTileCalls = 0;
    int ensureTileCalls = 0;

    const TileUpdateSelectionWorkResult result =
        TileUpdateSelectionWorkRunner::run(
            TileUpdateSelectionWorkInput{
                tilePlan,
                loadQueue,
                counters,
                reuseState,
                overlays,
                budget,
                &device,
                frameState,
                1,
                1,
                TileSelectionReuseMode::Strict,
                TileSelectionReuseRejectReason::None,
                true,
                16.0,
                true,
                1,
                true},
            [&]() {
                ++refreshCallCount;
                refreshSawReadyFill |=
                    tilePtr->content.renderContent.isFillReady();
            },
            [&](const FrameState&, TileSelectionPerformanceTimings&) {
                selectCalled = true;
            },
            [&](const TileKey& requestedKey) -> TilesetTile* {
                ++findTileCalls;
                auto it = tiles.find(testCacheKeyFor(requestedKey));
                return it == tiles.end() ? nullptr : it->second.get();
            },
            [&](const TileKey& requestedKey) -> TilesetTile* {
                ++ensureTileCalls;
                auto it = tiles.find(testCacheKeyFor(requestedKey));
                return it == tiles.end() ? nullptr : it->second.get();
            },
            [](TilesetTile&) {},
            [](TilesetTile&) {},
            [](TileLoadQueue&, FrameResourceBudget*) {
                return TileLoadRequestOutcome{};
            },
            [&](TilesetTile& changedTile) {
                EXPECT_EQ(tilePtr, &changedTile);
                tileResourcesChanged = true;
            });

    EXPECT_TRUE(result.reusedSelection);
    EXPECT_TRUE(refreshSawReadyFill);
    EXPECT_FALSE(selectCalled);
    EXPECT_TRUE(tilePtr->content.renderContent.isFillReady());
    EXPECT_TRUE(tileResourcesChanged);
    EXPECT_EQ(1, findTileCalls);
    EXPECT_EQ(0, ensureTileCalls);
    EXPECT_EQ(2, device.createdBufferCount);
    EXPECT_EQ(1, refreshCallCount);
}

TEST(
    TileUpdateSelectionWorkRunnerTest,
    MissingVisibleTileFallsBackToMaterialization) {
    TilePlan tilePlan;
    const TileKey key{"Geographic-TMS", 1, 0, 0};
    tilePlan.visibleTiles.push_back(key);
    TileLoadQueue loadQueue;
    TileSelectionCounters counters;
    TileSelectionReuseState reuseState;
    std::vector<ActivatedRasterOverlay*> overlays;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(45, config);
    FrameState frameState;
    frameState.frameId = 45;
    earth_engine::testing::MockRenderDevice device;
    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;

    int findTileCalls = 0;
    int ensureTileCalls = 0;
    TilesetTile* materializedTile = nullptr;

    TileUpdateSelectionWorkRunner::run(
        TileUpdateSelectionWorkInput{
            tilePlan,
            loadQueue,
            counters,
            reuseState,
            overlays,
            budget,
            &device,
            frameState,
            1,
            1,
            TileSelectionReuseMode::Strict,
            TileSelectionReuseRejectReason::None,
            true,
            16.0,
            true,
            1,
            true},
        []() {},
        [](const FrameState&, TileSelectionPerformanceTimings&) {},
        [&](const TileKey&) -> TilesetTile* {
            ++findTileCalls;
            return nullptr;
        },
        [&](const TileKey& requestedKey) -> TilesetTile* {
            ++ensureTileCalls;
            auto tile = std::make_unique<TilesetTile>(
                requestedKey,
                Rectangle::fromDegrees(-1.0, -1.0, 1.0, 1.0));
            materializedTile = tile.get();
            tiles.emplace(testCacheKeyFor(requestedKey), std::move(tile));
            return materializedTile;
        },
        [](TilesetTile&) {},
        [](TilesetTile&) {},
        [](TileLoadQueue&, FrameResourceBudget*) {
            return TileLoadRequestOutcome{};
        },
        [](TilesetTile&) {});

    ASSERT_NE(nullptr, materializedTile);
    EXPECT_TRUE(materializedTile->content.renderContent.isFillReady());
    EXPECT_EQ(1, findTileCalls);
    EXPECT_EQ(1, ensureTileCalls);
    EXPECT_EQ(2, device.createdBufferCount);
}

TEST(
    TileUpdateSelectionWorkRunnerTest,
    ReselectRefreshesRenderPlanAfterFillCreation) {
    TilePlan tilePlan;
    const TileKey key{"Geographic-TMS", 1, 0, 0};
    tilePlan.visibleTiles.push_back(key);
    TileLoadQueue loadQueue;
    TileSelectionCounters counters;
    TileSelectionReuseState reuseState;
    std::vector<ActivatedRasterOverlay*> overlays;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    budget.beginFrame(46, config);
    FrameState frameState;
    frameState.frameId = 46;
    earth_engine::testing::MockRenderDevice device;
    auto tile = std::make_unique<TilesetTile>(
        key,
        Rectangle::fromDegrees(-1.0, -1.0, 1.0, 1.0));
    TilesetTile* tilePtr = tile.get();

    bool selectCalled = false;
    int refreshCallCount = 0;
    bool refreshSawReadyFill = false;

    TileUpdateSelectionWorkRunner::run(
        TileUpdateSelectionWorkInput{
            tilePlan,
            loadQueue,
            counters,
            reuseState,
            overlays,
            budget,
            &device,
            frameState,
            1,
            1,
            TileSelectionReuseMode::None,
            TileSelectionReuseRejectReason::ResourceChanged,
            false,
            16.0,
            true,
            1,
            true},
        [&]() {
            ++refreshCallCount;
            refreshSawReadyFill |=
                tilePtr->content.renderContent.isFillReady();
        },
        [&](const FrameState&, TileSelectionPerformanceTimings&) {
            selectCalled = true;
        },
        [&](const TileKey&) -> TilesetTile* { return tilePtr; },
        [&](const TileKey&) -> TilesetTile* { return tilePtr; },
        [](TilesetTile&) {},
        [](TilesetTile&) {},
        [](TileLoadQueue&, FrameResourceBudget*) {
            return TileLoadRequestOutcome{};
        },
        [](TilesetTile&) {});

    EXPECT_TRUE(selectCalled);
    EXPECT_EQ(1, refreshCallCount);
    EXPECT_TRUE(refreshSawReadyFill);
    EXPECT_TRUE(tilePtr->content.renderContent.isFillReady());
}

TEST(
    TileUpdateSelectionWorkRunnerTest,
    StableVisibleFillSetDoesNotRebuildBuffersOrInvalidateSelection) {
    constexpr int kTileCount = 128;
    TilePlan tilePlan;
    TileLoadQueue loadQueue;
    TileSelectionCounters counters;
    TileSelectionReuseState reuseState;
    std::vector<ActivatedRasterOverlay*> overlays;
    FrameResourceBudgetConfig config;
    FrameResourceBudget budget;
    FrameState frameState;
    frameState.frameId = 50;
    earth_engine::testing::MockRenderDevice device;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    for (int i = 0; i < kTileCount; ++i) {
        const TileKey key{"Geographic-TMS", 8, i, 0};
        tilePlan.visibleTiles.push_back(key);
        auto tile = std::make_unique<TilesetTile>(
            key,
            Rectangle::fromDegrees(
                -180.0 + static_cast<double>(i),
                -1.0,
                -179.0 + static_cast<double>(i),
                1.0));
        tiles.emplace(testCacheKeyFor(key), std::move(tile));
    }

    TileContentCacheManager cache;
    uint64_t resourceRevision = 23;
    TileContentResourceInvalidator invalidator(
        resourceRevision,
        cache);
    int refreshCallCount = 0;
    int reconcileCallCount = 0;

    auto runFrame = [&]() {
        budget.beginFrame(frameState.frameId, config);
        return TileUpdateSelectionWorkRunner::run(
            TileUpdateSelectionWorkInput{
                tilePlan,
                loadQueue,
                counters,
                reuseState,
                overlays,
                budget,
                &device,
                frameState,
                resourceRevision,
                1,
                TileSelectionReuseMode::Strict,
                TileSelectionReuseRejectReason::None,
                true,
                16.0,
                true,
                1,
                true},
            [&]() { ++refreshCallCount; },
            [](const FrameState&, TileSelectionPerformanceTimings&) {},
            [&](const TileKey& key) -> TilesetTile* {
                const auto it = tiles.find(testCacheKeyFor(key));
                return it == tiles.end() ? nullptr : it->second.get();
            },
            [](const TileKey&) -> TilesetTile* { return nullptr; },
            [](TilesetTile&) {},
            [](TilesetTile&) {},
            [](TileLoadQueue&,
               FrameResourceBudget*) {
                return TileLoadRequestOutcome{};
            },
            [&](TilesetTile& tile) {
                ++reconcileCallCount;
                invalidator.reconcileTileResources(tile);
            });
    };

    runFrame();
    const int64_t firstFrameBytes = cache.totalBytesUsed();
    ASSERT_GT(firstFrameBytes, 0);
    EXPECT_EQ(kTileCount * 2, device.createdBufferCount);
    EXPECT_EQ(kTileCount, reconcileCallCount);
    EXPECT_EQ(1, refreshCallCount);
    EXPECT_EQ(23u, resourceRevision);

    ++frameState.frameId;
    runFrame();

    EXPECT_EQ(firstFrameBytes, cache.totalBytesUsed());
    EXPECT_EQ(kTileCount * 2, device.createdBufferCount);
    EXPECT_EQ(kTileCount, reconcileCallCount);
    EXPECT_EQ(1, refreshCallCount);
    EXPECT_EQ(23u, resourceRevision);
}
