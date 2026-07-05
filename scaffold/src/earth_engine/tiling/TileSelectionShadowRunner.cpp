#include "TileSelectionShadowRunner.h"

#include "RasterMappedToTilesetTile.h"
#include "TileContentAccess.h"
#include "TileKey.h"
#include "TileOcclusionState.h"
#include "TileScheme.h"
#include "TileSelectionFrameFinalizationRunner.h"
#include "TileSelectionFrameRunner.h"
#include "TileSelectionStateResetter.h"
#include "TileSelectionTraversalContextBuilder.h"
#include "TileSelectionTraversalExecutor.h"
#include "Tileset.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"

#include <vector>

namespace earth_engine {

namespace {

// Shadow per-visit hook: mirrors Tileset::onSelectionVisitTile — run the
// identical per-tile reset (shift selectionState → previousSelectionState,
// recompute renderability) and record the tile in this frame's active-set for
// finalize. The shadow tree is rebuilt each frame and each tile is visited at
// most once by the traversal, so no once-per-frame stamp guard is needed.
struct ShadowVisitState {
    const std::vector<ActivatedRasterOverlay*>* overlays;
    std::vector<TilesetTile*>* activeTiles;
};

void shadowOnVisitTile(void* userData, TilesetTile& tile) {
    auto* state = static_cast<ShadowVisitState*>(userData);
    TileSelectionStateResetter::resetOne(tile, *state->overlays);
    state->activeTiles->push_back(&tile);
}

TileOcclusionState shadowNotOccluded(void*, const TilesetTile&) {
    return TileOcclusionState::NotOccluded;
}

} // namespace

void TileSelectionShadowRunner::run(const TileSelectionShadowRunInput& input) {
    shadowTree_.build(input.liveRegistry);
    shadowFadingOut_.clear();
    shadowActiveTiles_.clear();
    shadowBudget_.beginFrame(input.frameState.frameId, FrameResourceBudgetConfig{});

    TileContentAccess shadowContentAccess =
        input.contentProviderOwnsTerrainQuadtree
            ? TileContentAccess::forContentTerrain(
                  shadowTree_.registry(),
                  input.tileScheme,
                  *input.contentProvider)
            : TileContentAccess::forNoTerrain(
                  shadowTree_.registry(),
                  input.tileScheme,
                  input.contentProvider);

    const TileSelectionTraversalContextBinding::CheckOcclusionFn occlusionFn =
        input.checkOcclusion ? input.checkOcclusion : shadowNotOccluded;

    TileSelectionFrameRunner::run(
        TileSelectionFrameRunInput{
            shadowTilePlan_,
            shadowLoadQueue_,
            shadowCounters_,
            input.frameState,
            input.options.fogDensityTable,
            input.tileScheme.id(),
            input.contentProvider ? input.contentProvider->rootTiles()
                                  : std::vector<TileKey>{},
            input.useVirtualTerrainRoot},
        // Frame-start reset: the shadow tree is fresh, so only the active-set
        // accumulator needs clearing (per-tile reset happens on visit).
        [this]() { shadowActiveTiles_.clear(); },
        [&shadowContentAccess](const TileKey& key) {
            return shadowContentAccess.ensureTile(key);
        },
        [&](TilesetTile& root, const SelectorFrame& selectorFrame) {
            ShadowVisitState visitState{&shadowOverlays_, &shadowActiveTiles_};
            TileSelectionTraversalContextBinding binding{
                shadowTilePlan_,
                shadowLoadQueue_,
                input.options,
                shadowOverlays_,
                shadowContentAccess,
                input.occlusionUserData,
                occlusionFn,
                nullptr};
            TileSelectionTraversalContext traversalContext =
                TileSelectionTraversalContextBuilder::build(
                    TileSelectionTraversalContextBuildInput{
                        shadowTilePlan_,
                        shadowLoadQueue_,
                        shadowCounters_,
                        input.options,
                        shadowOverlays_,
                        nullptr,
                        shadowBudget_,
                        input.lastCameraPosition,
                        shadowContentAccess},
                    binding);
            // owner is null → the builder leaves onVisitTile unset; wire the
            // shadow-local reset/active-set hook directly instead.
            traversalContext.onVisitTileFn = shadowOnVisitTile;
            traversalContext.onVisitTileUserData = &visitState;
            TileSelectionTraversalExecutor::visitTileIfNeeded(
                traversalContext,
                root,
                selectorFrame,
                0,
                false);
        },
        [&](const FrameState& finalizeFrameState) {
            return TileSelectionFrameFinalizationRunner::finalize(
                TileSelectionFrameFinalizationInput{
                    shadowTilePlan_,
                    shadowTree_.registry(),
                    shadowActiveTiles_,
                    shadowCounters_,
                    shadowContentAccess,
                    shadowFadingOut_,
                    shadowOverlays_,
                    finalizeFrameState.deltaSeconds,
                    TileLodTransitionFrameOptions{
                        input.options.enableLodTransitionPeriod,
                        input.options.lodTransitionLength},
                    TileRenderPlanFrameRefreshOptions{
                        input.options.enableLodTransitionPeriod,
                        input.interactionActive,
                        input.resourceSmoothingActive}});
        });
}

} // namespace earth_engine
