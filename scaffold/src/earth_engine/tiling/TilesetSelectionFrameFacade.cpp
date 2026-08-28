#include "TilesetSelectionFrameFacade.h"

#include "TileContentAccess.h"
#include "TileLoadQueue.h"
#include "TileOcclusionState.h"
#include "TileRenderPlanFrameRefresher.h"
#include "TileScheme.h"
#include "TileSelectionFrameFinalizationRunner.h"
#include "TileSelectionFrameRunner.h"
#include "TileSelectionTraversalContextBuilder.h"
#include "TileSelectionTraversalExecutor.h"
#include "Tileset.h"
#include "TilesetTile.h"

#include "../debug/PerfTimer.h"
#include "../scene/FrameState.h"

namespace earth_engine {

void TilesetSelectionFrameFacade::selectTiles(
    Tileset& tileset,
    const FrameState& frameState,
    TileSelectionPerformanceTimings* performanceTimings,
    IPrepareRendererResources* pPrepRenderer) {
    if (performanceTimings) {
        *performanceTimings = TileSelectionPerformanceTimings{};
        performanceTimings->collectDetailed =
            perf::shouldLog(frameState.frameId);
    }
    tileset.currentFrameTimeSeconds_ = frameState.timeSeconds;
    const auto& configuredOverlays = tileset.rasterOverlays();
    TileSelectionFrameRunner::run(
        TileSelectionFrameRunInput{
            tileset.tilePlan_,
            tileset.loadQueue_,
            tileset.selectionCounters_,
            frameState,
            tileset.options_.fogDensityTable,
            tileset.tileScheme_->id(),
            tileset.terrainProviders_.contentProvider()
                ? tileset.terrainProviders_.contentProvider()->rootTiles()
                : std::vector<TileKey>{},
            tileset.hasTerrainQuadtree(),
            performanceTimings},
        [&tileset]() {
            tileset.resetActiveSelectionState();
        },
        [&tileset](const TileKey& key) {
            return tileset.contentAccess_.ensureTile(key);
        },
        [&tileset, performanceTimings, pPrepRenderer](
            TilesetTile& root,
            const SelectorFrame& selectorFrame) {
            TileSelectionTraversalContextBinding binding{
                &tileset,
                [](void* userData, const TilesetTile& tile) {
                    return static_cast<Tileset*>(userData)
                        ->checkOcclusion(tile);
                },
                [](void* userData, TilesetTile& tile) {
                    static_cast<Tileset*>(userData)
                        ->onSelectionVisitTile(tile);
                },
                &tileset};
            TileSelectionTraversalContext traversalContext =
                TileSelectionTraversalContextBuilder::build(
                    TileSelectionTraversalContextBuildInput{
                        tileset.tilePlan_,
                        tileset.loadQueue_,
                        tileset.selectionCounters_,
                        tileset.options_,
                        tileset.device_,
                        pPrepRenderer,
                        tileset.frameResourceBudget_,
                        tileset.lastCameraPosition_,
                        tileset.contentAccess_,
                        performanceTimings,
                        tileset.rasterOverlayRuntime_.frameContext()},
                    binding);
            TileSelectionTraversalExecutor::visitTileIfNeeded(
                traversalContext,
                root,
                selectorFrame,
                0,
                false);
        },
        [&tileset, &configuredOverlays](const FrameState&) {
            return TileSelectionFrameFinalizationRunner::finalize(
                TileSelectionFrameFinalizationInput{
                    tileset.tilePlan_,
                    tileset.tileRegistry_,
                    tileset.selectionActiveTiles_,
                    tileset.selectionActiveTilesPrev_,
                    tileset.selectionCounters_,
                    tileset.contentAccess_,
                    configuredOverlays,
                    TileRenderPlanFrameRefreshOptions{
                        tileset.interactionActiveForFrame_,
                        tileset.resourceSmoothingActiveForFrame_,
                        tileset.options_.maximumScreenSpaceError,
                        tileset.options_.seamEdgeMismatchProbe,
                        -1,
                        tileset.rasterOverlayRuntime_.frameContext()}});
        });
}

} // namespace earth_engine
