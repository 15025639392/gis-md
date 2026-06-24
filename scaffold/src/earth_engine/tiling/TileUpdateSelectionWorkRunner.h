#pragma once

#include "TileLoadQueue.h"
#include "TileLoadTypes.h"
#include "TilePlan.h"
#include "TileRasterOverlayFrameProcessor.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileSelectionCounters.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TileSelectionReuseState.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../renderer/RenderDevice.h"
#include "../scene/FrameState.h"
#include "../debug/PerfTimer.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace earth_engine {

struct TileUpdateSelectionWorkInput {
    TilePlan& tilePlan;
    TileLoadQueue& loadQueue;
    TileSelectionCounters& selectionCounters;
    TileSelectionReuseState& selectionReuseState;
    std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    FrameResourceBudget& frameResourceBudget;
    RenderDevice* device = nullptr;
    const FrameState& frameState;
    uint64_t currentResourceRevision = 0;
    uint64_t currentOverlaySignature = 0;
    TileSelectionReuseMode reuseMode = TileSelectionReuseMode::None;
    TileSelectionReuseRejectReason reuseRejectReason =
        TileSelectionReuseRejectReason::None;
    bool reusedSelection = false;
    double maximumScreenSpaceError = 16.0;
};

struct TileUpdateSelectionWorkResult {
    double computeMs = 0.0;
    double prefetchMs = 0.0;
    double requestMs = 0.0;
    TileSelectionReuseMode reuseMode = TileSelectionReuseMode::None;
    TileSelectionReuseRejectReason reuseRejectReason =
        TileSelectionReuseRejectReason::None;
    bool reusedSelection = false;
};

class TileUpdateSelectionWorkRunner {
public:
    template <typename RefreshTilePlanRenderEntriesFn,
              typename SelectTilesFn,
              typename EnsureTileFn,
              typename UnloadTileContentFn,
              typename RequestMissingTilesFn>
    static TileUpdateSelectionWorkResult run(
        TileUpdateSelectionWorkInput input,
        RefreshTilePlanRenderEntriesFn&& refreshTilePlanRenderEntries,
        SelectTilesFn&& selectTiles,
        EnsureTileFn&& ensureTile,
        UnloadTileContentFn&& unloadTileContent,
        RequestMissingTilesFn&& requestMissingTiles) {
        TileUpdateSelectionWorkResult result;
        result.reuseMode = input.reuseMode;
        result.reuseRejectReason = input.reuseRejectReason;
        result.reusedSelection =
            input.reuseMode != TileSelectionReuseMode::None ||
            input.reusedSelection;

        const double computeStartMs = perf::nowMs();
        if (input.reusedSelection) {
            input.tilePlan.frameId = input.frameState.frameId;
            input.selectionCounters.reset();
            refreshTilePlanRenderEntries();
        } else {
            selectTiles(input.frameState);
            input.selectionReuseState.commit(
                input.frameState,
                input.currentResourceRevision,
                input.currentOverlaySignature);
        }
        result.computeMs = perf::nowMs() - computeStartMs;

        const double prefetchStartMs = perf::nowMs();
        const std::vector<size_t> overlayOrder =
            TileSelectionRasterOverlayPreparer::processingOrder(
                input.rasterOverlays);
        TileRasterOverlayFrameProcessor::prefetchSelection(
            input.tilePlan,
            input.loadQueue.requests(),
            input.rasterOverlays,
            overlayOrder,
            input.device,
            input.maximumScreenSpaceError,
            input.frameResourceBudget,
            ensureTile,
            nullptr,
            std::forward<UnloadTileContentFn>(unloadTileContent));
        result.prefetchMs = perf::nowMs() - prefetchStartMs;

        const double requestStartMs = perf::nowMs();
        TileLoadRequestOutcome requestOutcome;
        requestOutcome = requestMissingTiles(
            input.loadQueue.requests(),
            &input.frameResourceBudget);
        input.selectionReuseState.recordRequestOutcome(
            requestOutcome.issued > 0,
            requestOutcome.blockedByInflight);
        result.requestMs = perf::nowMs() - requestStartMs;

        return result;
    }
};

} // namespace earth_engine
