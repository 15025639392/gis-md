#pragma once

#include "TileLoadQueue.h"
#include "TileLoadTypes.h"
#include "TilePlan.h"
#include "TileRasterOverlayFrameProcessor.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileSelectionCounters.h"
#include "TileFillProxyPreparer.h"
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
    bool enableTerrainFillProxy = false;
    int terrainFillProxyGridSize = 16;
    bool hasTerrainQuadtree = false;
    const std::unordered_map<std::string, std::unique_ptr<TilesetTile>>* tiles =
        nullptr;
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

        // During reuse, the tile plan hasn't changed so rerunning the full
        // overlay prefetch is redundant.  Individual tile overlay states
        // (missing projections, failed tiles) are handled lazily: they will
        // be detected on the next non-reuse frame's prefetch or by the
        // overlay provider's incremental tile update system.  Matching
        // cesium-native where updateTileOverlays runs outside selection.
        if (!input.reusedSelection) {
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
                std::forward<UnloadTileContentFn>(unloadTileContent),
                [&input](const TileKey& key,
                         TileLoadPriorityGroup group,
                         double priority) {
                    input.loadQueue.queue(key, group, priority);
                });
            result.prefetchMs = perf::nowMs() - prefetchStartMs;
        }

        // Terrain fill proxy: give each visible tile still lacking real terrain
        // a drape-ready ellipsoid proxy so imagery appears on the smooth globe
        // immediately (cesium-js TerrainFillMesh model). Runs after prefetch
        // (imagery mappings advanced) and before refreshRenderEntries so
        // canBuildRenderEntryDirectly picks up freshly-ready fills this frame.
        // Flag-gated (default off) → golden/existing behavior unchanged.
        if (input.enableTerrainFillProxy && input.hasTerrainQuadtree &&
            !input.reusedSelection &&
            input.tiles != nullptr) {
            const std::vector<size_t> fillOverlayOrder =
                TileSelectionRasterOverlayPreparer::processingOrder(
                    input.rasterOverlays);
            for (const TileKey& key : input.tilePlan.visibleTiles) {
                TilesetTile* tile = ensureTile(key);
                if (!tile) continue;
                const bool madeFill = TileFillProxyPreparer::ensureFillProxy(
                    *tile,
                    *input.tiles,
                    input.device,
                    input.terrainFillProxyGridSize);
                // Map imagery onto a fill tile (keyed on its bounding-volume
                // rectangle — the proxy's own rectangle). The normal Done-path
                // raster update never runs for a still-loading fill tile, so
                // drive the mapping ONCE here (mappingCount==0); subsequent
                // frames advance it via the prefetch pass above
                // (advanceThrottledLoads), so this is not per-frame work.
                if (tile->content.renderContent.drawsFill() &&
                    tile->rasterOverlayState.mappingCount() == 0 &&
                    !input.rasterOverlays.empty()) {
                    TileRasterOverlayPrefetcher::prefetch(
                        *tile,
                        input.rasterOverlays,
                        fillOverlayOrder,
                        input.device,
                        input.maximumScreenSpaceError,
                        input.frameResourceBudget);
                }
                (void)madeFill;
            }
        }

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
