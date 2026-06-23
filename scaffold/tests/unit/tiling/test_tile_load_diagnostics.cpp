#include <gtest/gtest.h>

#include "earth_engine/core/resources/FrameResourceBudget.h"
#include "earth_engine/tiling/RasterMappedToTilesetTile.h"
#include "earth_engine/tiling/TileLoadDiagnostics.h"
#include "earth_engine/tiling/TileLoadLifecycle.h"
#include "earth_engine/tiling/TileLoadQueue.h"
#include "earth_engine/tiling/TileUnloadQueue.h"
#include "earth_engine/tiling/TilesetTile.h"

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

using namespace earth_engine;

namespace {

std::unique_ptr<TilesetTile> makeTile(
    const std::string& id,
    TileLoadState loadState,
    TileContentKind contentKind) {
    auto tile = std::make_unique<TilesetTile>(
        TileKey{id, 0, 0, 0},
        Rectangle{});
    tile->content.loadState = loadState;
    tile->content.contentKind = contentKind;
    return tile;
}

} // namespace

TEST(
    TileLoadDiagnosticsCollectorTest,
    CountsQueuesLifecycleBudgetAndTiles) {
    TileLoadQueue loadQueue;
    loadQueue.queue(
        TileKey{"test", 0, 0, 0},
        TileLoadPriorityGroup::Preload,
        3.0);
    loadQueue.queue(
        TileKey{"test", 0, 0, 1},
        TileLoadPriorityGroup::Normal,
        2.0);
    loadQueue.queue(
        TileKey{"test", 0, 1, 0},
        TileLoadPriorityGroup::Urgent,
        1.0);

    TileLoadLifecycle lifecycle;
    CancellationToken terrainToken;
    CancellationToken contentToken;
    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().beginTerrainRequest(
            "terrain-request",
            terrainToken);
        lifecycle.requestState().beginContentRequest(
            "content-request",
            contentToken);
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
            TileKey{"test", 1, 0, 0},
            "terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableTerrain()});
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::LegacyHeightmapTerrain,
                TileKey{"test", 1, 0, 1},
                "terrain-terminal",
                TileLoadPriorityGroup::Urgent,
                0.0,
                TileLoadStatus::Empty});
        lifecycle.pendingLoads().addUpload(PendingTileLoad{
            TileLoadDomain::GltfTerrain,
            TileKey{"test", 1, 0, 2},
            "gltf-terrain-upload",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadResult::createRenderableGltfTerrain(
                std::make_unique<GltfModel>())});
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{
            TileLoadDomain::GltfTerrain,
            TileKey{"test", 1, 0, 3},
            "gltf-terrain-terminal",
            TileLoadPriorityGroup::Normal,
            0.0,
            TileLoadStatus::RetryLater});
        lifecycle.pendingLoads().addUpload(PendingTileLoad{TileLoadDomain::Content,
            TileKey{"test", 1, 1, 0},
            "content-upload",
            TileLoadPriorityGroup::Urgent,
            0.0,
            TileLoadResult::fromContentResult(TileContentLoadResult::empty())});
        lifecycle.pendingLoads().addTerminalResult(PendingTileLoad{TileLoadDomain::Content,
                TileKey{"test", 1, 1, 1},
                "content-terminal",
                TileLoadPriorityGroup::Normal,
                0.0,
                TileLoadStatus::Failed});
    }

    TileUnloadQueue unloadQueue;
    unloadQueue.pushBackIfAbsent("unload-a");
    unloadQueue.pushBackIfAbsent("unload-b");

    FrameResourceBudgetConfig budgetConfig;
    budgetConfig.maxNetworkRequestsPerFrame = 10;
    budgetConfig.maxTerrainContentNetworkRequestsPerFrame = 3;
    budgetConfig.maxRasterNetworkRequestsPerFrame = 4;
    budgetConfig.maxMainThreadFinalizesPerFrame = 2;
    budgetConfig.maxTerminalStateTransitionsPerFrame = 5;
    budgetConfig.maxRasterUploadsPerFrame = 6;
    budgetConfig.mainThreadTimeMs = 7.0;
    budgetConfig.interactionActive = true;
    budgetConfig.smoothingActive = true;

    FrameResourceBudget resourceBudget;
    resourceBudget.beginFrame(42, budgetConfig);
    EXPECT_TRUE(resourceBudget.tryIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Normal));
    EXPECT_TRUE(resourceBudget.tryIssue(
        FrameResourceLane::ContentRequest,
        FrameResourcePriority::Normal));
    EXPECT_TRUE(resourceBudget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal,
        2));
    EXPECT_TRUE(resourceBudget.tryFinalize(
        FrameResourceLane::TerrainFinalize,
        FrameResourcePriority::Normal));
    EXPECT_TRUE(resourceBudget.tryFinalize(
        FrameResourceLane::TerminalState,
        FrameResourcePriority::Normal));
    EXPECT_TRUE(resourceBudget.tryFinalize(
        FrameResourceLane::RasterTextureUpload,
        FrameResourcePriority::Normal));
    resourceBudget.recordElapsed(FrameResourceLane::ContentFinalize, 1.5);

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles["unloaded"] = makeTile(
        "unloaded",
        TileLoadState::Unloaded,
        TileContentKind::Unknown);
    tiles["done"] = makeTile(
        "done",
        TileLoadState::Done,
        TileContentKind::Render);
    tiles["done"]->rasterOverlayState.missingProjections().push_back(
        RasterOverlayProjection::Geographic);
    tiles["null"] = nullptr;

    const TilesetLoadDiagnostics diag =
        TileLoadDiagnosticsCollector::collect(
            loadQueue,
            lifecycle,
            resourceBudget,
            unloadQueue,
            tiles);

    EXPECT_EQ(diag.loadQueuePreloadRequests, 1);
    EXPECT_EQ(diag.loadQueueNormalRequests, 1);
    EXPECT_EQ(diag.loadQueueUrgentRequests, 1);
    EXPECT_EQ(diag.loadQueueTotal(), 3);

    EXPECT_EQ(diag.pendingTerrainRequests, 1);
    EXPECT_EQ(diag.pendingTerrainUploads, 1);
    EXPECT_EQ(diag.pendingTerrainTerminalResults, 1);
    EXPECT_EQ(diag.pendingGltfTerrainUploads, 1);
    EXPECT_EQ(diag.pendingGltfTerrainTerminalResults, 1);
    EXPECT_EQ(diag.pendingContentRequests, 1);
    EXPECT_EQ(diag.pendingContentUploads, 1);
    EXPECT_EQ(diag.pendingContentTerminalResults, 1);
    EXPECT_EQ(diag.pendingTerrainTotal(), 5);
    EXPECT_EQ(diag.pendingContentTotal(), 3);

    EXPECT_EQ(diag.unloadQueueTiles, 2);
    EXPECT_EQ(diag.loadUnloadedTiles, 1);
    EXPECT_EQ(diag.loadDoneTiles, 1);
    EXPECT_EQ(diag.contentUnknownTiles, 1);
    EXPECT_EQ(diag.contentRenderTiles, 1);
    EXPECT_EQ(diag.missingRasterOverlayProjections, 1);

    EXPECT_EQ(diag.resourceBudget.frameNumber, 42u);
    EXPECT_EQ(diag.resourceBudget.networkRequestsIssued, 4u);
    EXPECT_EQ(diag.resourceBudget.terrainContentNetworkRequestsIssued, 2u);
    EXPECT_EQ(diag.resourceBudget.rasterNetworkRequestsIssued, 2u);
    EXPECT_EQ(diag.resourceBudget.mainThreadFinalizesUsed, 1u);
    EXPECT_EQ(diag.resourceBudget.terminalStateTransitionsUsed, 1u);
    EXPECT_EQ(diag.resourceBudget.rasterUploadsUsed, 1u);
    EXPECT_EQ(
        diag.resourceBudget.maxTerrainContentNetworkRequestsPerFrame,
        3u);
    EXPECT_EQ(diag.resourceBudget.maxRasterNetworkRequestsPerFrame, 4u);
    EXPECT_EQ(diag.resourceBudget.maxMainThreadFinalizesPerFrame, 2u);
    EXPECT_EQ(
        diag.resourceBudget.maxTerminalStateTransitionsPerFrame,
        5u);
    EXPECT_EQ(diag.resourceBudget.maxRasterUploadsPerFrame, 6u);
    EXPECT_NEAR(diag.resourceBudget.mainThreadElapsedMs, 1.5, 1e-12);
    EXPECT_EQ(diag.resourceBudget.mainThreadTimeMs, 7.0);
    EXPECT_TRUE(diag.resourceBudget.interactionActive);
    EXPECT_TRUE(diag.resourceBudget.smoothingActive);

    {
        std::lock_guard<std::mutex> lock(lifecycle.mutex());
        lifecycle.requestState().completeTerrainRequest("terrain-request");
        lifecycle.requestState().completeContentRequest("content-request");
    }
}

TEST(
    TileLoadDiagnosticsCollectorTest,
    CountsEveryLoadStateAndContentKind) {
    TileLoadQueue loadQueue;
    TileLoadLifecycle lifecycle;
    FrameResourceBudget resourceBudget;
    TileUnloadQueue unloadQueue;

    std::unordered_map<std::string, std::unique_ptr<TilesetTile>> tiles;
    tiles["unloading"] = makeTile(
        "unloading",
        TileLoadState::Unloading,
        TileContentKind::Unknown);
    tiles["failed-temporarily"] = makeTile(
        "failed-temporarily",
        TileLoadState::FailedTemporarily,
        TileContentKind::Empty);
    tiles["unloaded"] = makeTile(
        "unloaded",
        TileLoadState::Unloaded,
        TileContentKind::External);
    tiles["content-loading"] = makeTile(
        "content-loading",
        TileLoadState::ContentLoading,
        TileContentKind::Render);
    tiles["content-loaded"] = makeTile(
        "content-loaded",
        TileLoadState::ContentLoaded,
        TileContentKind::Unknown);
    tiles["done"] = makeTile(
        "done",
        TileLoadState::Done,
        TileContentKind::Render);
    tiles["failed"] = makeTile(
        "failed",
        TileLoadState::Failed,
        TileContentKind::Unknown);

    const TilesetLoadDiagnostics diag =
        TileLoadDiagnosticsCollector::collect(
            loadQueue,
            lifecycle,
            resourceBudget,
            unloadQueue,
            tiles);

    EXPECT_EQ(diag.loadUnloadingTiles, 1);
    EXPECT_EQ(diag.loadFailedTemporarilyTiles, 1);
    EXPECT_EQ(diag.loadUnloadedTiles, 1);
    EXPECT_EQ(diag.loadContentLoadingTiles, 1);
    EXPECT_EQ(diag.loadContentLoadedTiles, 1);
    EXPECT_EQ(diag.loadDoneTiles, 1);
    EXPECT_EQ(diag.loadFailedTiles, 1);

    EXPECT_EQ(diag.contentUnknownTiles, 3);
    EXPECT_EQ(diag.contentEmptyTiles, 1);
    EXPECT_EQ(diag.contentExternalTiles, 1);
    EXPECT_EQ(diag.contentRenderTiles, 2);
}
