#pragma once

#include "TileRenderEntryCommandBuilder.h"
#include "TileRenderFrameMaintenance.h"
#include "TileSelectionCounters.h"
#include "TileSelectionReusePolicy.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdint>

namespace earth_engine {

struct TileUpdateDebugLogInput {
    size_t renderTileCount = 0;
    size_t loadRequestCount = 0;
    double selectorMs = 0.0;
    double prefetchMs = 0.0;
    double requestMs = 0.0;
    double terrainUploadMs = 0.0;
    double rasterUploadMs = 0.0;
    size_t terrainCacheSize = 0;
    size_t pendingRequestCount = 0;
    TileSelectionCounters selectionCounters;
    TileSelectionReuseMode reuseMode = TileSelectionReuseMode::None;
    TileSelectionReuseRejectReason reuseRejectReason =
        TileSelectionReuseRejectReason::None;
    bool reusedSelection = false;
    int rasterUploadsProcessed = 0;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
};

struct TileRenderDebugLogInput {
    double selectedBuildMs = 0.0;
    double fadeBuildMs = 0.0;
    TileRenderFrameMaintenanceTimings maintenanceTimings;
    size_t selectedTileCount = 0;
    size_t fadingTileCount = 0;
    TileRenderEntryCommandStats renderStats;
    size_t commandCount = 0;
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
    int synchronousRenderPrepCount = 0;
    int deferredRenderPrepCount = 0;
    int ancestorFallbackDrawCount = 0;
};

class TileFrameDebugLogFormatter {
public:
    static std::array<char, 384> updateDetail(
        const TileUpdateDebugLogInput& input) {
        std::array<char, 384> detail{};
        std::snprintf(
            detail.data(),
            detail.size(),
            "render=%zu load=%zu selector=%.2f prefetch=%.2f request=%.2f terrainUpload=%.2f rasterUpload=%.2f cache=%zu pending=%zu visited=%d culled=%d culledVisited=%d fog=%d occluded=%d occWait=%d kicked=%d notReady=%d reused=%d reuseMode=%d reuseReject=%d rasterUploads=%d interaction=%d smoothing=%d",
            input.renderTileCount,
            input.loadRequestCount,
            input.selectorMs,
            input.prefetchMs,
            input.requestMs,
            input.terrainUploadMs,
            input.rasterUploadMs,
            input.terrainCacheSize,
            input.pendingRequestCount,
            input.selectionCounters.visited,
            input.selectionCounters.culled,
            input.selectionCounters.culledVisited,
            input.selectionCounters.fogCulled,
            input.selectionCounters.occluded,
            input.selectionCounters.waitingForOcclusionResults,
            input.selectionCounters.kicked,
            input.selectionCounters.notYetRenderable,
            input.reusedSelection ? 1 : 0,
            static_cast<int>(input.reuseMode),
            static_cast<int>(input.reuseRejectReason),
            input.rasterUploadsProcessed,
            input.interactionActive ? 1 : 0,
            input.resourceSmoothingActive ? 1 : 0);
        return detail;
    }

    static std::array<char, 384> renderBuildDetail(
        const TileRenderDebugLogInput& input) {
        std::array<char, 384> detail{};
        std::snprintf(
            detail.data(),
            detail.size(),
            "selected=%.2f fade=%.2f detach=%.2f trim=%.2f eligible=%.2f bytes=%.2f unload=%.2f selectedTiles=%zu fadeTiles=%zu ensured=%d cmds=%zu interaction=%d smoothing=%d prepSync=%d prepDeferred=%d fallback=%d",
            input.selectedBuildMs,
            input.fadeBuildMs,
            input.maintenanceTimings.detachInactiveMs,
            input.maintenanceTimings.trimRasterMs,
            input.maintenanceTimings.eligibilityMs,
            input.maintenanceTimings.cacheBytesMs,
            input.maintenanceTimings.unloadMs,
            input.selectedTileCount,
            input.fadingTileCount,
            input.renderStats.ensuredTiles,
            input.commandCount,
            input.interactionActive ? 1 : 0,
            input.resourceSmoothingActive ? 1 : 0,
            input.synchronousRenderPrepCount,
            input.deferredRenderPrepCount,
            input.ancestorFallbackDrawCount);
        return detail;
    }
};

} // namespace earth_engine
