#pragma once

#include "../debug/PerfTimer.h"

namespace earth_engine {

struct TileRenderFrameMaintenanceTimings {
    double detachInactiveMs = 0.0;
    double trimRasterMs = 0.0;
    double eligibilityMs = 0.0;
    double cacheBytesMs = 0.0;
    double unloadMs = 0.0;
};

class TileRenderFrameMaintenance {
public:
    template <typename TrimRasterCachesFn,
              typename ShouldUnloadCachedBytesFn,
              typename UnloadCachedBytesFn>
    static TileRenderFrameMaintenanceTimings run(
        TrimRasterCachesFn&& trimRasterCaches,
        ShouldUnloadCachedBytesFn&& shouldUnloadCachedBytes,
        UnloadCachedBytesFn&& unloadCachedBytes) {
        TileRenderFrameMaintenanceTimings timings;

        const double trimRasterStartMs = perf::nowMs();
        trimRasterCaches(false);
        timings.trimRasterMs = perf::nowMs() - trimRasterStartMs;

        const double unloadStartMs = perf::nowMs();
        if (shouldUnloadCachedBytes()) {
            unloadCachedBytes();
        }
        timings.unloadMs = perf::nowMs() - unloadStartMs;

        return timings;
    }
};

} // namespace earth_engine
