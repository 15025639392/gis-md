#include "TileSelectionTraversalContextBuilder.h"

#include "TileContentAccess.h"
#include "TileSelectionPlanAppender.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TileSelectionTraversalDetailsBuilder.h"
#include "Tileset.h"
#include "TilesetTile.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"

namespace earth_engine {

namespace {

void queueTileLoad(void* userData,
                   const TileKey& key,
                   TileLoadPriorityGroup group,
                   double priority) {
    auto& binding =
        *static_cast<TileSelectionTraversalContextBinding*>(userData);
    TileSelectionPlanAppender::queueTileLoad(
        binding.loadQueue,
        key,
        group,
        priority);
}

void addTileToCurrentPlan(void* userData,
                          TilesetTile& tile,
                          double screenSpaceError,
                          bool queueForLoad,
                          double priority) {
    auto& binding =
        *static_cast<TileSelectionTraversalContextBinding*>(userData);
    TileSelectionPlanAppender::addTileToCurrentPlan(
        binding.tilePlan,
        binding.loadQueue,
        binding.options.enableLodTransitionPeriod,
        tile,
        screenSpaceError,
        queueForLoad,
        priority);
}

TileChildFrameMaterializeResult ensureTileChildren(void* userData,
                                                   TilesetTile& tile) {
    return static_cast<TileContentAccess*>(userData)->ensureTileChildren(tile);
}

bool canRefine(void* userData, const TilesetTile& tile) {
    return static_cast<TileContentAccess*>(userData)->canRefine(tile);
}

TileOcclusionState checkOcclusion(void* userData, const TilesetTile& tile) {
    auto& binding =
        *static_cast<TileSelectionTraversalContextBinding*>(userData);
    return binding.checkOcclusion(binding.occlusionUserData, tile);
}

bool hasLodTransitionRenderContent(void* userData, const TilesetTile& tile) {
    const auto& binding =
        *static_cast<TileSelectionTraversalContextBinding*>(userData);
    return tile.content.contentKind == TileContentKind::Render &&
           TileSelectionRasterOverlayPreparer::isRenderable(
               tile,
               binding.rasterOverlays);
}

TileTraversalDetails createSingleTileDetails(
    void* userData,
    const TilesetTile& tile) {
    const auto& binding =
        *static_cast<TileSelectionTraversalContextBinding*>(userData);
    return TileSelectionTraversalDetailsBuilder::forSingleTile(
        tile,
        binding.rasterOverlays);
}

TileTraversalDetails createCulledTileDetails(
    void* userData,
    const TilesetTile& tile) {
    const auto& binding =
        *static_cast<TileSelectionTraversalContextBinding*>(userData);
    return TileSelectionTraversalDetailsBuilder::forCulledTile(
        tile,
        binding.rasterOverlays,
        binding.options.forbidHoles);
}

void onVisitTile(void* userData, TilesetTile& tile) {
    static_cast<Tileset*>(userData)->onSelectionVisitTile(tile);
}

} // namespace

TileSelectionTraversalContext TileSelectionTraversalContextBuilder::build(
    TileSelectionTraversalContextBuildInput input,
    TileSelectionTraversalContextBinding& binding) {
    // Precompute the camera cartographic once per frame; the traversal reuses
    // it for every visited tile instead of resolving it iteratively each time.
    const Cartographic cameraCart =
        Ellipsoid::WGS84().cartesianToCartographic(input.lastCameraPosition);
    return TileSelectionTraversalContext{
        input.tilePlan,
        input.loadQueue,
        input.counters,
        input.options,
        input.rasterOverlays,
        input.device,
        input.frameResourceBudget,
        input.lastCameraPosition,
        cameraCart.longitude(),
        cameraCart.latitude(),
        &binding,
        &input.contentAccess,
        queueTileLoad,
        addTileToCurrentPlan,
        ensureTileChildren,
        canRefine,
        checkOcclusion,
        hasLodTransitionRenderContent,
        createSingleTileDetails,
        createCulledTileDetails,
        binding.owner ? onVisitTile : nullptr,
        binding.owner};
}

} // namespace earth_engine
