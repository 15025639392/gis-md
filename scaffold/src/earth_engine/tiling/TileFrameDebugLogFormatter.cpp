#include "TileFrameDebugLogFormatter.h"

#include <cstdio>

namespace earth_engine {

std::array<char, 640> TileFrameDebugLogFormatter::updateDetail(
    const TileUpdateDebugLogInput& input) {
    std::array<char, 640> detail{};
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

std::array<char, 640> TileFrameDebugLogFormatter::renderBuildDetail(
    const TileRenderDebugLogInput& input) {
    std::array<char, 640> detail{};
    std::snprintf(
        detail.data(),
        detail.size(),
        "selected=%.2f fade=%.2f detach=%.2f trim=%.2f eligible=%.2f bytes=%.2f unload=%.2f selectedTiles=%zu fadeTiles=%zu entries=%d selectedEntries=%d fadeEntries=%d ensured=%d cmds=%zu selectedCmds=%d fadeCmds=%d missed=%d selectedMissed=%d fadeMissed=%d missingSelected=%d missingRender=%d deferred=%d selectedDeferred=%d fadeDeferred=%d interaction=%d smoothing=%d prepSync=%d prepDeferred=%d fallback=%d",
        input.selectedBuildMs,
        input.fadeBuildMs,
        input.maintenanceTimings.detachInactiveMs,
        input.maintenanceTimings.trimRasterMs,
        input.maintenanceTimings.eligibilityMs,
        input.maintenanceTimings.cacheBytesMs,
        input.maintenanceTimings.unloadMs,
        input.selectedTileCount,
        input.fadingTileCount,
        input.renderStats.plannedEntries,
        input.selectedRenderStats.plannedEntries,
        input.fadingRenderStats.plannedEntries,
        input.renderStats.ensuredTiles,
        input.commandCount,
        input.selectedRenderStats.drawAttempts,
        input.fadingRenderStats.drawAttempts,
        input.renderStats.missedDrawEntries,
        input.selectedRenderStats.missedDrawEntries,
        input.fadingRenderStats.missedDrawEntries,
        input.renderStats.missingSelectedTiles,
        input.renderStats.missingRenderTiles,
        input.renderStats.deferredEntries,
        input.selectedRenderStats.deferredEntries,
        input.fadingRenderStats.deferredEntries,
        input.interactionActive ? 1 : 0,
        input.resourceSmoothingActive ? 1 : 0,
        input.synchronousRenderPrepCount,
        input.deferredRenderPrepCount,
        input.ancestorFallbackDrawCount);
    return detail;
}

} // namespace earth_engine
