#include "ProviderRequestDiagnosticsAggregator.h"

#include <algorithm>

namespace earth_engine {

void ProviderRequestDiagnosticsAggregator::add(
    ProviderRequestDiagnostics& total,
    const ProviderRequestDiagnostics& next) {
    total.requestsStarted += next.requestsStarted;
    total.requestsCompleted += next.requestsCompleted;
    total.activeWorkerBlockingRequests +=
        next.activeWorkerBlockingRequests;
    total.peakWorkerBlockingRequests =
        std::max(total.peakWorkerBlockingRequests,
                 next.peakWorkerBlockingRequests);
    total.externalResourceRequestsStarted +=
        next.externalResourceRequestsStarted;
    total.externalResourceRequestsCompleted +=
        next.externalResourceRequestsCompleted;
    total.activeExternalResourceBlockingRequests +=
        next.activeExternalResourceBlockingRequests;
    total.peakExternalResourceBlockingRequests =
        std::max(total.peakExternalResourceBlockingRequests,
                 next.peakExternalResourceBlockingRequests);
    if (next.maximumTransportActiveRequests >= 0) {
        total.maximumTransportActiveRequests =
            std::max(total.maximumTransportActiveRequests,
                     next.maximumTransportActiveRequests);
    }
}

} // namespace earth_engine
