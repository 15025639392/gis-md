#pragma once

namespace earth_engine {

struct ProviderRequestDiagnostics {
    int requestsStarted = 0;
    int requestsCompleted = 0;
    int requestsFailed = 0;
    int activeWorkerBlockingRequests = 0;
    int peakWorkerBlockingRequests = 0;
    int externalResourceRequestsStarted = 0;
    int externalResourceRequestsCompleted = 0;
    int externalResourceRequestsFailed = 0;
    int activeExternalResourceBlockingRequests = 0;
    int peakExternalResourceBlockingRequests = 0;
    int maximumTransportActiveRequests = -1;
};

} // namespace earth_engine
