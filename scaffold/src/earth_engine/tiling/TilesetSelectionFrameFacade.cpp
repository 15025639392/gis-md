#include "TilesetSelectionFrameFacade.h"

#include "TileContentAccess.h"
#include "TileLoadQueue.h"
#include "TileLodTransitionFrameUpdater.h"
#include "TileRenderPlanFinalizer.h"
#include "TileSelectionPlanAppender.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TileSelectionFrameRunner.h"
#include "TileSelectionStateResetter.h"
#include "TileSelectionTraversalDetailsBuilder.h"
#include "TileSelectionTraversalContext.h"
#include "TileSelectionTraversalExecutor.h"
#include "Tileset.h"

#include "../scene/FrameState.h"

namespace earth_engine {

namespace {

constexpr int kActiveInteractionRenderPrepBudget = 0;
constexpr int kRecoveryRenderPrepBudget = 1;

} // namespace

void TilesetSelectionFrameFacade::refreshTilePlanRenderEntries(
    Tileset& tileset) {
    TileRenderPlanFinalizer::refreshRenderEntries(
        tileset.tilePlan_,
        TileRenderPlanFinalizeOptions{
            tileset.options_.enableLodTransitionPeriod,
            tileset.interactionActiveForFrame_,
            tileset.resourceSmoothingActiveForFrame_,
            kActiveInteractionRenderPrepBudget,
            kRecoveryRenderPrepBudget},
            [&tileset](const TileKey& key) {
                return tileset.contentAccess_.ensureTile(key);
            },
            [](const TileKey& key) {
                return TileCacheKey::forTile(key);
            });
}

TileSelectionFrameFinalizeTimings
TilesetSelectionFrameFacade::finalizeSelectedTilePlan(
    Tileset& tileset,
    const FrameState& frameState) {
    return TileSelectionFrameFinalizer::finalize(
        tileset.tilePlan_,
        tileset.tileRegistry_.tiles(),
        tileset.selectionCounters_,
        frameState.deltaSeconds,
        [&tileset](double deltaSeconds) {
            TileLodTransitionFrameUpdater::update(
                tileset.tilePlan_,
                tileset.tileRegistry_,
                tileset.tilesFadingOut_,
                tileset.rasterOverlays_,
                deltaSeconds,
                TileLodTransitionFrameOptions{
                    tileset.options_.enableLodTransitionPeriod,
                    tileset.options_.lodTransitionLength});
        },
        [&tileset]() {
            refreshTilePlanRenderEntries(tileset);
        },
        [&tileset](const TilesetTile& tile) {
            return TileSelectionRasterOverlayPreparer::isRenderable(
                tile,
                tileset.rasterOverlays_);
        });
}

void TilesetSelectionFrameFacade::selectTiles(
    Tileset& tileset,
    const FrameState& frameState) {
    tileset.currentFrameTimeSeconds_ = frameState.timeSeconds;
    TileSelectionFrameRunner::run(
        TileSelectionFrameRunInput{
            tileset.tilePlan_,
            tileset.loadQueue_,
            tileset.selectionCounters_,
            frameState,
            tileset.options_.fogDensityTable,
            tileset.tileScheme_->id(),
            tileset.contentProvider_ ? tileset.contentProvider_->rootTiles()
                                     : std::vector<TileKey>{}},
        [&tileset]() {
            TileSelectionStateResetter::reset(
                tileset.tileRegistry_,
                tileset.rasterOverlays_);
        },
        [&tileset](const TileKey& key) {
            return tileset.contentAccess_.ensureTile(key);
        },
        [&tileset](TilesetTile& root, const SelectorFrame& selectorFrame) {
            TileSelectionTraversalContext traversalContext{
                tileset.tilePlan_,
                tileset.loadQueue_,
                tileset.selectionCounters_,
                tileset.options_,
                tileset.rasterOverlays_,
                tileset.device_,
                tileset.frameResourceBudget_,
                tileset.lastCameraPosition_,
                &tileset,
                &tileset.contentAccess_,
                [](void* userData,
                   const TileKey& key,
                   TileLoadPriorityGroup group,
                   double priority) {
                    Tileset& tileset = *static_cast<Tileset*>(userData);
                    TileSelectionPlanAppender::queueTileLoad(
                        tileset.loadQueue_,
                        key,
                        group,
                        priority);
                },
                [](void* userData,
                   TilesetTile& tile,
                   double screenSpaceError,
                   bool queueForLoad,
                   double priority) {
                    Tileset& tileset = *static_cast<Tileset*>(userData);
                    TileSelectionPlanAppender::addTileToCurrentPlan(
                        tileset.tilePlan_,
                        tileset.loadQueue_,
                        tileset.options_.enableLodTransitionPeriod,
                        tile,
                        screenSpaceError,
                        queueForLoad,
                        priority);
                },
                [](void* userData, TilesetTile& tile) {
                    static_cast<TileContentAccess*>(userData)
                        ->ensureTileChildren(tile);
                },
                [](void* userData, const TilesetTile& tile) {
                    return static_cast<TileContentAccess*>(userData)
                        ->canRefine(tile);
                },
                [](void* userData, const TilesetTile& tile) {
                    return static_cast<Tileset*>(userData)
                        ->checkOcclusion(tile);
                },
                [](void* userData, const TilesetTile& tile) {
                    const Tileset& tileset =
                        *static_cast<Tileset*>(userData);
                    return tile.content.contentKind ==
                               TileContentKind::Render &&
                           TileSelectionRasterOverlayPreparer::isRenderable(
                               tile,
                               tileset.rasterOverlays_);
                },
                [](void* userData, const TilesetTile& tile) {
                    const Tileset& tileset =
                        *static_cast<Tileset*>(userData);
                    return TileSelectionTraversalDetailsBuilder::forSingleTile(
                        tile,
                        tileset.rasterOverlays_);
                },
                [](void* userData, const TilesetTile& tile) {
                    const Tileset& tileset =
                        *static_cast<Tileset*>(userData);
                    return TileSelectionTraversalDetailsBuilder::forCulledTile(
                        tile,
                        tileset.rasterOverlays_,
                        tileset.options_.forbidHoles);
                }};
            TileSelectionTraversalExecutor::visitTileIfNeeded(
                traversalContext,
                root,
                selectorFrame,
                0,
                false);
        },
        [&tileset](const FrameState& finalizeFrameState) {
            return finalizeSelectedTilePlan(tileset, finalizeFrameState);
        });
}

} // namespace earth_engine
