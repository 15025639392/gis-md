#pragma once

#include "TileLoadQueue.h"
#include "TilePlan.h"
#include "TileSelectionCounters.h"
#include "TileSelectionFrameBuilder.h"
#include "TileSelectionRootPolicy.h"
#include "../debug/PerfTimer.h"
#include "../scene/FrameState.h"

#include <string>
#include <vector>

namespace earth_engine {

struct FrameState;
struct TileKey;
struct TilesetTile;

struct TileSelectionFrameRunInput {
    TilePlan& tilePlan;
    TileLoadQueue& loadQueue;
    TileSelectionCounters& selectionCounters;
    const FrameState& frameState;
    const std::vector<FogDensityAtHeight>& fogDensityTable;
    std::string tileSchemeId;
    std::vector<TileKey> explicitRoots;
};

class TileSelectionFrameRunner {
public:
    template <typename ResetTileSelectionStateFn,
              typename EnsureTileFn,
              typename VisitTileIfNeededFn,
              typename FinalizeSelectedTilePlanFn>
    static void run(
        TileSelectionFrameRunInput input,
        ResetTileSelectionStateFn&& resetTileSelectionState,
        EnsureTileFn&& ensureTile,
        VisitTileIfNeededFn&& visitTileIfNeeded,
        FinalizeSelectedTilePlanFn&& finalizeSelectedTilePlan) {
        const double selectorStartMs = perf::nowMs();
        input.tilePlan = TilePlan{};
        input.tilePlan.frameId = input.frameState.frameId;
        input.loadQueue.clear();
        input.selectionCounters.reset();

        const double resetStartMs = perf::nowMs();
        resetTileSelectionState();
        const double resetMs = perf::nowMs() - resetStartMs;

        const double viewsStartMs = perf::nowMs();
        const SelectorFrame selectorFrame =
            TileSelectionFrameBuilder::build(
                input.frameState,
                input.fogDensityTable);
        const double viewsMs = perf::nowMs() - viewsStartMs;
        if (selectorFrame.views.empty()) {
#ifndef __ANDROID__
            (void)selectorStartMs;
            (void)resetMs;
            (void)viewsMs;
#endif
            return;
        }

        const double traversalStartMs = perf::nowMs();
        const std::vector<TileKey> roots =
            TileSelectionRootPolicy::chooseRoots(
                input.tileSchemeId,
                input.explicitRoots);
        for (const TileKey& key : roots) {
            TilesetTile* root = ensureTile(key);
            if (root) {
                visitTileIfNeeded(*root, selectorFrame);
            }
        }
        const double traversalMs = perf::nowMs() - traversalStartMs;

        const auto finalizeTimings = finalizeSelectedTilePlan(
            input.frameState);

#ifndef __ANDROID__
        (void)selectorStartMs;
        (void)resetMs;
        (void)viewsMs;
        (void)traversalMs;
        (void)finalizeTimings;
#endif
    }
};

} // namespace earth_engine
