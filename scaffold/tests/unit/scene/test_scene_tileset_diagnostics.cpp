#include <gtest/gtest.h>

#include "earth_engine/scene/SceneTilesetDiagnostics.h"

#include <cmath>

using namespace earth_engine;

TEST(
    SceneProviderRequestDiagnosticsSnapshotTest,
    AggregatesProviderLanes) {
    ProviderRequestDiagnostics first;
    first.requestsStarted = 2;
    first.requestsCompleted = 1;
    first.activeWorkerBlockingRequests = 1;
    first.peakWorkerBlockingRequests = 3;
    first.maximumTransportActiveRequests = 8;
    first.externalResourceRequestsStarted = 4;
    first.externalResourceRequestsCompleted = 2;
    first.activeExternalResourceBlockingRequests = 1;
    first.peakExternalResourceBlockingRequests = 5;

    ProviderRequestDiagnostics second;
    second.requestsStarted = 5;
    second.requestsCompleted = 4;
    second.activeWorkerBlockingRequests = 2;
    second.peakWorkerBlockingRequests = 2;
    second.maximumTransportActiveRequests = 11;
    second.externalResourceRequestsStarted = 3;
    second.externalResourceRequestsCompleted = 3;
    second.activeExternalResourceBlockingRequests = 2;
    second.peakExternalResourceBlockingRequests = 4;

    SceneProviderRequestDiagnosticsSnapshot snapshot =
        SceneProviderRequestDiagnosticsSnapshot::fromProvider(first);
    snapshot.add(
        SceneProviderRequestDiagnosticsSnapshot::fromProvider(second));

    EXPECT_EQ(snapshot.requestsStarted, 7);
    EXPECT_EQ(snapshot.requestsCompleted, 5);
    EXPECT_EQ(snapshot.activeWorkerBlockingRequests, 3);
    EXPECT_EQ(snapshot.peakWorkerBlockingRequests, 3);
    EXPECT_EQ(snapshot.transportActiveRequestLimit, 11);
    EXPECT_EQ(snapshot.externalResourceRequestsStarted, 7);
    EXPECT_EQ(snapshot.externalResourceRequestsCompleted, 5);
    EXPECT_EQ(snapshot.activeExternalResourceBlockingRequests, 3);
    EXPECT_EQ(snapshot.peakExternalResourceBlockingRequests, 5);
}

TEST(
    SceneFrameResourceBudgetDiagnosticsSnapshotTest,
    AggregatesBudgetLanes) {
    FrameResourceBudgetConfig firstConfig;
    firstConfig.maxNetworkRequestsPerFrame = 20;
    firstConfig.maxTerrainContentNetworkRequestsPerFrame = 20;
    firstConfig.maxRasterNetworkRequestsPerFrame = 32;
    firstConfig.maxNetworkInflight = 10;
    firstConfig.maxTerrainContentNetworkInflight = 10;
    firstConfig.maxRasterNetworkInflight = 16;
    firstConfig.maxMainThreadFinalizesPerFrame = 3;
    firstConfig.maxTerminalStateTransitionsPerFrame = 8;
    firstConfig.maxRasterUploadsPerFrame = 4;
    firstConfig.mainThreadTimeMs = 2.5;
    firstConfig.interactionActive = true;

    FrameResourceBudget firstBudget;
    firstBudget.beginFrame(10, firstConfig);
    firstBudget.tryIssue(
        FrameResourceLane::TerrainRequest,
        FrameResourcePriority::Normal);
    firstBudget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal,
        4);
    firstBudget.tryFinalize(
        FrameResourceLane::TerrainFinalize,
        FrameResourcePriority::Normal);
    firstBudget.tryFinalize(
        FrameResourceLane::RasterTextureUpload,
        FrameResourcePriority::Normal,
        2);
    firstBudget.recordElapsed(FrameResourceLane::TerrainFinalize, 0.75);

    FrameResourceBudgetConfig secondConfig;
    secondConfig.maxNetworkRequestsPerFrame = 6;
    secondConfig.maxTerrainContentNetworkRequestsPerFrame = 6;
    secondConfig.maxRasterNetworkRequestsPerFrame = 12;
    secondConfig.maxNetworkInflight = 4;
    secondConfig.maxTerrainContentNetworkInflight = 4;
    secondConfig.maxRasterNetworkInflight = 7;
    secondConfig.maxMainThreadFinalizesPerFrame = 2;
    secondConfig.maxTerminalStateTransitionsPerFrame = 3;
    secondConfig.maxRasterUploadsPerFrame = 2;
    secondConfig.mainThreadTimeMs = 4.0;
    secondConfig.smoothingActive = true;

    FrameResourceBudget secondBudget;
    secondBudget.beginFrame(11, secondConfig);
    secondBudget.tryIssue(
        FrameResourceLane::ContentRequest,
        FrameResourcePriority::Normal,
        2);
    secondBudget.tryIssue(
        FrameResourceLane::RasterRequest,
        FrameResourcePriority::Normal,
        3);
    secondBudget.tryFinalize(
        FrameResourceLane::TerminalState,
        FrameResourcePriority::Normal);
    secondBudget.recordElapsed(FrameResourceLane::ContentFinalize, 1.25);

    SceneFrameResourceBudgetDiagnosticsSnapshot snapshot =
        SceneFrameResourceBudgetDiagnosticsSnapshot::fromBudget(
            firstBudget.snapshot());
    snapshot.add(SceneFrameResourceBudgetDiagnosticsSnapshot::fromBudget(
        secondBudget.snapshot()));

    EXPECT_EQ(snapshot.networkRequestsIssued, 10);
    EXPECT_EQ(snapshot.networkRequestsLimit, 26);
    EXPECT_EQ(snapshot.terrainContentNetworkRequestsIssued, 3);
    EXPECT_EQ(snapshot.terrainContentNetworkRequestsLimit, 26);
    EXPECT_EQ(snapshot.rasterNetworkRequestsIssued, 7);
    EXPECT_EQ(snapshot.rasterNetworkRequestsLimit, 44);
    EXPECT_EQ(snapshot.networkInflightLimit, 14);
    EXPECT_EQ(snapshot.terrainContentNetworkInflightLimit, 14);
    EXPECT_EQ(snapshot.rasterNetworkInflightLimit, 23);
    EXPECT_EQ(snapshot.mainThreadFinalizesUsed, 1);
    EXPECT_EQ(snapshot.mainThreadFinalizesLimit, 5);
    EXPECT_EQ(snapshot.terminalStateTransitionsUsed, 1);
    EXPECT_EQ(snapshot.terminalStateTransitionsLimit, 11);
    EXPECT_EQ(snapshot.rasterUploadsUsed, 2);
    EXPECT_EQ(snapshot.rasterUploadsLimit, 6);
    EXPECT_LT(std::abs(snapshot.mainThreadElapsedMs - 2.0), 1e-12);
    EXPECT_EQ(snapshot.mainThreadTimeLimitMs, 4.0);
    EXPECT_TRUE(snapshot.interactionActive);
    EXPECT_TRUE(snapshot.smoothingActive);
}
