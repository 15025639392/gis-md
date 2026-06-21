#include "TilesetSelectionFrameFacade.h"

#include "TileContentAccess.h"
#include "TileLoadQueue.h"
#include "TileSelectionFrameFinalizationRunner.h"
#include "TileSelectionFrameRunner.h"
#include "TileSelectionStateResetter.h"
#include "TileSelectionTraversalContextBuilder.h"
#include "TileSelectionTraversalExecutor.h"
#include "Tileset.h"

#include "../scene/FrameState.h"

namespace earth_engine {

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
                                     : std::vector<TileKey>{},
            tileset.terrainProvider_ != nullptr},
        [&tileset]() {
            TileSelectionStateResetter::reset(
                tileset.tileRegistry_,
                tileset.rasterOverlays_);
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
                }};
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
