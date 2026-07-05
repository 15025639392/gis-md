#include "TilesetSelectionFrameFacade.h"

#include "TileContentAccess.h"
#include "TileLoadQueue.h"
#include "TileOcclusionState.h"
#include "TileScheme.h"
#include "TileSelectionFrameFinalizationRunner.h"
#include "TileSelectionFrameRunner.h"
#include "TileSelectionShadowRunner.h"
#include "TileSelectionTraversalContextBuilder.h"
#include "TileSelectionTraversalExecutor.h"
#include "Tileset.h"
#include "TilesetTile.h"

#include "../scene/FrameState.h"

#include <utility>

namespace earth_engine {

void TilesetSelectionFrameFacade::selectTiles(
    Tileset& tileset,
    const FrameState& frameState) {
    tileset.currentFrameTimeSeconds_ = frameState.timeSeconds;
    if (tileset.options_.asyncSelection) {
        selectTilesAsyncShadow(tileset, frameState);
        return;
    }
    selectTilesSync(tileset, frameState);
}

TileOcclusionState TilesetSelectionFrameFacade::shadowOcclusion(
    void* tilesetPtr,
    const TilesetTile& tile) {
    return static_cast<Tileset*>(tilesetPtr)->checkOcclusion(tile);
}

void TilesetSelectionFrameFacade::selectTilesAsyncShadow(
    Tileset& tileset,
    const FrameState& frameState) {
    // Run the ordinary selection traversal on a shadow copy of the live tree.
    // The shadow result is byte-identical to the sync path (same executor over a
    // faithful read-surface mirror); see TileSelectionShadowRunner.
    TileSelectionShadowRunner runner;
    runner.run(TileSelectionShadowRunInput{
        tileset.tileRegistry_,
        *tileset.tileScheme_,
        tileset.terrainProviders_.contentProvider(),
        tileset.terrainProviders_.contentProviderOwnsTerrainQuadtree(),
        tileset.hasTerrainQuadtree(),
        tileset.options_,
        frameState,
        tileset.lastCameraPosition_,
        tileset.interactionActiveForFrame_,
        tileset.resourceSmoothingActiveForFrame_,
        &shadowOcclusion,
        &tileset});

    // Reconcile the shadow result onto live. Render/load keys and counters are
    // moved/copied wholesale; each shadow tile's final selection state is
    // written back to the corresponding live tile so the next frame's shadow
    // (seeded from live) carries correct cross-frame selection history.
    //
    // For golden, every selected key already exists in the live registry (the
    // scene is pre-materialized). Materializing live tiles that only the shadow
    // virtual-descended is part of the later real-path apply and is not needed
    // by the content-less oracle.
    tileset.tilePlan_ = std::move(runner.tilePlan());
    tileset.loadQueue_ = runner.loadQueue();
    tileset.selectionCounters_ = runner.counters();

    for (const auto& entry : runner.shadowTree().registry().tiles()) {
        const TilesetTile* shadowTile = entry.second.get();
        if (!shadowTile) {
            continue;
        }
        TilesetTile* liveTile = tileset.tileRegistry_.findTile(shadowTile->key);
        if (!liveTile) {
            continue;
        }
        liveTile->selectionFrameState.selectionState =
            shadowTile->selectionFrameState.selectionState;
        liveTile->selectionFrameState.previousSelectionState =
            shadowTile->selectionFrameState.previousSelectionState;
    }
}

void TilesetSelectionFrameFacade::selectTilesSync(
    Tileset& tileset,
    const FrameState& frameState) {
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
            tileset.hasTerrainQuadtree()},
        [&tileset]() {
            tileset.resetActiveSelectionState();
        },
        [&tileset](const TileKey& key) {
            return tileset.contentAccess_.ensureTile(key);
        },
        [&tileset](TilesetTile& root, const SelectorFrame& selectorFrame) {
            TileSelectionTraversalContextBinding binding{
                tileset.tilePlan_,
                tileset.loadQueue_,
                tileset.options_,
                tileset.rasterOverlays_,
                tileset.contentAccess_,
                &tileset,
                [](void* userData, const TilesetTile& tile) {
                    return static_cast<Tileset*>(userData)
                        ->checkOcclusion(tile);
                },
                &tileset};
            TileSelectionTraversalContext traversalContext =
                TileSelectionTraversalContextBuilder::build(
                    TileSelectionTraversalContextBuildInput{
                        tileset.tilePlan_,
                        tileset.loadQueue_,
                        tileset.selectionCounters_,
                        tileset.options_,
                        tileset.rasterOverlays_,
                        tileset.device_,
                        tileset.frameResourceBudget_,
                        tileset.lastCameraPosition_,
                        tileset.contentAccess_},
                    binding);
            TileSelectionTraversalExecutor::visitTileIfNeeded(
                traversalContext,
                root,
                selectorFrame,
                0,
                false);
        },
        [&tileset](const FrameState& finalizeFrameState) {
            return TileSelectionFrameFinalizationRunner::finalize(
                TileSelectionFrameFinalizationInput{
                    tileset.tilePlan_,
                    tileset.tileRegistry_,
                    tileset.selectionActiveTiles_,
                    tileset.selectionCounters_,
                    tileset.contentAccess_,
                    tileset.tilesFadingOut_,
                    tileset.rasterOverlays_,
                    finalizeFrameState.deltaSeconds,
                    TileLodTransitionFrameOptions{
                        tileset.options_.enableLodTransitionPeriod,
                        tileset.options_.lodTransitionLength},
                    TileRenderPlanFrameRefreshOptions{
                        tileset.options_.enableLodTransitionPeriod,
                        tileset.interactionActiveForFrame_,
                        tileset.resourceSmoothingActiveForFrame_}});
        });
}

} // namespace earth_engine
