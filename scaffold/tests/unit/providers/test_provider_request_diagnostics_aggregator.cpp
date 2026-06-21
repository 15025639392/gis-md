#include <gtest/gtest.h>

#include "earth_engine/providers/ProviderRequestDiagnosticsAggregator.h"

using namespace earth_engine;

TEST(ProviderRequestDiagnosticsAggregatorTest, CombinesCountsAndPeaks) {
    ProviderRequestDiagnostics total;
    total.requestsStarted = 1;
    total.requestsCompleted = 1;
    total.activeWorkerBlockingRequests = 2;
    total.peakWorkerBlockingRequests = 7;
    total.externalResourceRequestsStarted = 3;
    total.externalResourceRequestsCompleted = 2;
    total.activeExternalResourceBlockingRequests = 4;
    total.peakExternalResourceBlockingRequests = 6;
    total.maximumTransportActiveRequests = -1;

    ProviderRequestDiagnostics first;
    first.requestsStarted = 5;
    first.requestsCompleted = 4;
    first.activeWorkerBlockingRequests = 3;
    first.peakWorkerBlockingRequests = 5;
    first.externalResourceRequestsStarted = 7;
    first.externalResourceRequestsCompleted = 6;
    first.activeExternalResourceBlockingRequests = 2;
    first.peakExternalResourceBlockingRequests = 9;
    first.maximumTransportActiveRequests = -1;

    ProviderRequestDiagnostics second;
    second.requestsStarted = 11;
    second.requestsCompleted = 10;
    second.activeWorkerBlockingRequests = 1;
    second.peakWorkerBlockingRequests = 8;
    second.externalResourceRequestsStarted = 13;
    second.externalResourceRequestsCompleted = 12;
    second.activeExternalResourceBlockingRequests = 5;
    second.peakExternalResourceBlockingRequests = 4;
    second.maximumTransportActiveRequests = 11;

    ProviderRequestDiagnosticsAggregator::add(total, first);
    ProviderRequestDiagnosticsAggregator::add(total, second);

    EXPECT_EQ(total.requestsStarted, 17);
    EXPECT_EQ(total.requestsCompleted, 15);
    EXPECT_EQ(total.activeWorkerBlockingRequests, 6);
    EXPECT_EQ(total.peakWorkerBlockingRequests, 8);
    EXPECT_EQ(total.maximumTransportActiveRequests, 11);
    EXPECT_EQ(total.externalResourceRequestsStarted, 23);
    EXPECT_EQ(total.externalResourceRequestsCompleted, 20);
    EXPECT_EQ(total.activeExternalResourceBlockingRequests, 11);
    EXPECT_EQ(total.peakExternalResourceBlockingRequests, 9);
}
