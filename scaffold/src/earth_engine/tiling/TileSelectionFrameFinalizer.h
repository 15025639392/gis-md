#pragma once

#include "TilePlan.h"
#include "TileSelectionCounters.h"
#include "TileSelectionSummaryBuilder.h"
#include "TileVisibleRangeFinalizer.h"
#include "../debug/PerfTimer.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct TilesetTile;

struct TileSelectionFrameFinalizeTimings {
    double dedupeMs = 0.0;
    double transitionMs = 0.0;
    double summaryMs = 0.0;
};

class TileSelectionFrameFinalizer {
public:
    template <typename UpdateLodTransitionsFn,
              typename RefreshRenderEntriesFn,
              typename IsTileRenderableFn>
    static TileSelectionFrameFinalizeTimings finalize(
        TilePlan& tilePlan,
        const std::unordered_map<
            std::string,
            std::unique_ptr<TilesetTile>>& tiles,
        TileSelectionCounters& selectionCounters,
        double deltaSeconds,
        UpdateLodTransitionsFn&& updateLodTransitions,
        RefreshRenderEntriesFn&& refreshRenderEntries,
        IsTileRenderableFn&& isTileRenderable) {
        TileSelectionFrameFinalizeTimings timings;

        const double dedupeStartMs = perf::nowMs();
        TileVisibleRangeFinalizer::dedupeVisibleTiles(tilePlan);
        timings.dedupeMs = perf::nowMs() - dedupeStartMs;

        const double transitionStartMs = perf::nowMs();
        updateLodTransitions(deltaSeconds);
        timings.transitionMs = perf::nowMs() - transitionStartMs;

        const double summaryStartMs = perf::nowMs();
        refreshRenderEntries();
        TileVisibleRangeFinalizer::updateVisibleZoomRange(tilePlan);

        const TileSelectionSummaryBuildResult summaryBuildResult =
            TileSelectionSummaryBuilder::build(
                tilePlan,
                tiles,
                TileSelectionSummaryBuildInput{
                    selectionCounters.culled,
                    selectionCounters.fogCulled,
                    selectionCounters.occluded,
                    selectionCounters.waitingForOcclusionResults,
                    selectionCounters.culledVisited},
                isTileRenderable);
        selectionCounters.notYetRenderable +=
            summaryBuildResult.notYetRenderableCount;
        timings.summaryMs = perf::nowMs() - summaryStartMs;
        return timings;
    }
};

} // namespace earth_engine
