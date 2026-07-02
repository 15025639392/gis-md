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
    bool useVirtualTerrainRoot = false;
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
        [[maybe_unused]] const double selectorStartMs = perf::nowMs();
        input.tilePlan = TilePlan{};
        input.tilePlan.frameId = input.frameState.frameId;
        input.loadQueue.clear();
        input.selectionCounters.reset();

        const double resetStartMs = perf::nowMs();
        resetTileSelectionState();
        [[maybe_unused]] const double resetMs = perf::nowMs() - resetStartMs;

        const double viewsStartMs = perf::nowMs();
        const SelectorFrame selectorFrame =
            TileSelectionFrameBuilder::build(
                input.frameState,
                input.fogDensityTable);
        [[maybe_unused]] const double viewsMs = perf::nowMs() - viewsStartMs;
        if (selectorFrame.views.empty()) {
            return;
        }

        const double traversalStartMs = perf::nowMs();
        const std::vector<TileKey> roots =
            TileSelectionRootPolicy::chooseRoots(
                input.tileSchemeId,
                input.explicitRoots,
                input.useVirtualTerrainRoot);
        for (const TileKey& key : roots) {
            TilesetTile* root = ensureTile(key);
            if (root) {
                visitTileIfNeeded(*root, selectorFrame);
            }
        }
        [[maybe_unused]] const double traversalMs = perf::nowMs() - traversalStartMs;

        [[maybe_unused]] const auto finalizeTimings = finalizeSelectedTilePlan(
            input.frameState);
    }
};

} // namespace earth_engine
