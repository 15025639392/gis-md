#include <gtest/gtest.h>

#include "earth_engine/scene/SceneTilesetDiagnostics.h"

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
