#pragma once

#include "ProviderRequestDiagnostics.h"

namespace earth_engine {

class ProviderRequestDiagnosticsAggregator {
public:
    static void add(ProviderRequestDiagnostics& total,
                    const ProviderRequestDiagnostics& next);
};

} // namespace earth_engine
