#include "TilesetSelectionFrameFacade.h"

#include "TileContentAccess.h"
#include "TileLoadQueue.h"
#include "TileOcclusionState.h"
#include "TileRenderPlanFrameRefresher.h"
#include "TileScheme.h"
#include "TileSelectionFrameFinalizationRunner.h"
#include "TileSelectionFrameRunner.h"
#include "TileSelectionShadowRunner.h"
#include "TileSelectionWorker.h"
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
    if (tileset.options_.asyncSelectionNonBlocking) {
        selectTilesAsyncWorker(tileset, frameState);
    } else {
        selectTilesSyncShadow(tileset, frameState);
    }
}

void TilesetSelectionFrameFacade::reconcileShadowToLive(
    Tileset& tileset,
    const TileSelectionShadowRunner& runner) {
    // Copy the shadow's SELECTION DECISION wholesale (visibleTiles / fading /
    // selectionRecords / counters / load keys). These carry only TileKeys and
    // plain values — no shadow TilesetTile* leaks into live state.
    tileset.tilePlan_ = runner.tilePlan();
    tileset.loadQueue_ = runner.loadQueue();
    tileset.selectionCounters_ = runner.counters();

    // Write each shadow tile's final selection state back to its live tile so
    // the next frame's shadow (seeded from live) carries correct cross-frame
    // selection history.
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

    // Re-resolve the render entries against LIVE content on the render thread.
    // The shadow ran finalize over content-less tiles, so its renderEntries /
    // credits / progress are empty — useless on device. This live pass:
    //   * MATERIALIZES any live tile the shadow only virtual-descended (live
    //     ensureTile() creates it from the scheme + links parents/children),
    //   * gates each render entry on the LIVE tile's real render content
    //     (isGltfRenderReady() / hasSurfaceDrawable()), falling back to the
    //     nearest renderable live ancestor with a surface-clip window, and
    //   * refreshes frame credits / load progress from live tiles.
    // Golden only compares visibleTiles + loadQueue (both copied above), so this
    // pass is invisible to the oracle while making the async path drawable.
    //
    // NOTE (documented limitation): per-tile lodTransitionFadePercentage is not
    // mirrored shadow→live, so LOD-transition fade opacity is only faithful when
    // options_.enableLodTransitionPeriod is false (the on-device default).
    TileRenderPlanFrameRefresher::refresh(
        tileset.tilePlan_,
        tileset.contentAccess_,
        tileset.rasterOverlays_,
        TileRenderPlanFrameRefreshOptions{
            tileset.options_.enableLodTransitionPeriod,
            tileset.interactionActiveForFrame_,
            tileset.resourceSmoothingActiveForFrame_});
}

void TilesetSelectionFrameFacade::selectTilesSyncShadow(
    Tileset& tileset,
    const FrameState& frameState) {
    // Run the ordinary selection traversal on a shadow copy of the live tree,
    // synchronously on the render thread. Byte-identical to the sync path (same
    // executor over a faithful read-surface mirror), so golden verifies it
    // frame-by-frame. Occlusion is safe here (no concurrent live mutation).
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
    reconcileShadowToLive(tileset, runner);
}

void TilesetSelectionFrameFacade::consumeAsyncSelectionResult(
    Tileset& tileset) {
    // Consuming a finished worker selection must NOT be gated behind the frame's
    // reuse decision. On a static camera the coordinator reuses every frame and
    // never calls selectTiles (where dispatch lives), yet the result from the
    // initial dispatch still needs to land — otherwise the plan stays empty,
    // nothing loads, the resource revision never advances, and the reuse gate
    // self-perpetuates (bootstrap deadlock, observed on-device: visited=0
    // forever). Reconciling here each frame breaks the cycle; dispatch stays
    // gated in selectTilesAsyncWorker so a converged scene still stops
    // re-selecting.
    if (!tileset.options_.asyncSelection ||
        !tileset.options_.asyncSelectionNonBlocking ||
        !tileset.selectionWorker_) {
        return;
    }
    if (const TileSelectionShadowRunner* runner =
            tileset.selectionWorker_->tryTakeResult()) {
        reconcileShadowToLive(tileset, *runner);
    }
}

void TilesetSelectionFrameFacade::selectTilesAsyncWorker(
    Tileset& tileset,
    const FrameState& frameState) {
    // True async: the heavy traversal runs on a dedicated worker while the
    // render thread proceeds. Selection results lag ≥1 frame; when the worker
    // is still busy this frame simply reuses the last reconciled plan.
    if (!tileset.selectionWorker_) {
        tileset.selectionWorker_ = std::make_unique<TileSelectionWorker>();
    }
    TileSelectionWorker& worker = *tileset.selectionWorker_;

    // 1. Consume a finished selection. Usually already taken this frame by
    //    consumeAsyncSelectionResult (the unconditional pre-step); tryTakeResult
    //    is idempotent, so this is a no-op then. Kept for the reuse-off case
    //    where this path is the only consumer.
    if (const TileSelectionShadowRunner* runner = worker.tryTakeResult()) {
        reconcileShadowToLive(tileset, *runner);
    }

    // 2. If the worker is idle, snapshot live + kick a new selection. Build the
    //    shadow here (render thread) so the worker never reads live; the input
    //    is a value snapshot so nothing per-frame outlives this call. Occlusion
    //    is disabled on the worker (a real thunk reads live mutable state).
    if (!worker.isBusy()) {
        worker.buildShadow(tileset.tileRegistry_);
        worker.dispatch(TileSelectionShadowSelectInput{
            tileset.tileScheme_.get(),
            tileset.terrainProviders_.contentProvider(),
            tileset.terrainProviders_.contentProviderOwnsTerrainQuadtree(),
            tileset.hasTerrainQuadtree(),
            &tileset.options_,
            frameState,
            tileset.lastCameraPosition_,
            tileset.interactionActiveForFrame_,
            tileset.resourceSmoothingActiveForFrame_,
            nullptr,
            nullptr});
    }
    // 3. else: worker busy → keep the last reconciled tilePlan_ (fallback).
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
