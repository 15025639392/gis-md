#pragma once

#include "TileFrameState.h"

#include "../debug/PerfTimer.h"
#include "../layers/ActivatedRasterOverlay.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

struct TilesetTile;

struct TileRenderFrameMaintenanceTimings {
    double detachInactiveMs = 0.0;
    double trimRasterMs = 0.0;
    double eligibilityMs = 0.0;
    double cacheBytesMs = 0.0;
    double unloadMs = 0.0;
};

class TileRenderFrameMaintenance {
public:
    template <typename DetachInactiveFn,
              typename MarkEligibleFn,
              typename UpdateTotalBytesFn,
              typename UnloadCachedBytesFn>
    static TileRenderFrameMaintenanceTimings run(
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        uint64_t frameNumber,
        std::vector<ActivatedRasterOverlay*>& rasterOverlays,
        bool& cacheBytesDirty,
        bool resourceSmoothingActive,
        bool shouldUnloadCachedBytes,
        DetachInactiveFn&& detachInactive,
        MarkEligibleFn&& markEligible,
        UpdateTotalBytesFn&& updateTotalBytes,
        UnloadCachedBytesFn&& unloadCachedBytes) {
        TileRenderFrameMaintenanceTimings timings;

        const std::vector<TileFrameInactiveEntry> inactiveTiles =
            TileFrameState::collectInactiveTiles(tiles, frameNumber);

        const double detachInactiveStartMs = perf::nowMs();
        detachInactive(inactiveTiles);
        timings.detachInactiveMs = perf::nowMs() - detachInactiveStartMs;

        const double trimRasterStartMs = perf::nowMs();
        for (auto* overlay : rasterOverlays) {
            if (overlay) {
                overlay->trimUnusedTiles();
            }
        }
        timings.trimRasterMs = perf::nowMs() - trimRasterStartMs;

        const double eligibilityStartMs = perf::nowMs();
        for (const TileFrameInactiveEntry& entry : inactiveTiles) {
            markEligible(entry.cacheKey);
        }
        timings.eligibilityMs = perf::nowMs() - eligibilityStartMs;

        const double cacheBytesStartMs = perf::nowMs();
        if (cacheBytesDirty) {
            updateTotalBytes();
            cacheBytesDirty = false;
        }
        timings.cacheBytesMs = perf::nowMs() - cacheBytesStartMs;

        const double unloadStartMs = perf::nowMs();
        if (!resourceSmoothingActive && shouldUnloadCachedBytes) {
            unloadCachedBytes();
        }
        timings.unloadMs = perf::nowMs() - unloadStartMs;

        return timings;
    }
};

} // namespace earth_engine
