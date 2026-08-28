#include "TileSelectionTraversalContextBuilder.h"

// TileContentAccess.h completes DirectRasterMapping, which the context's
// value type transitively needs instantiated; keep it even though no symbol is
// named directly here.
#include "TileContentAccess.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"

namespace earth_engine {

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
        input.device,
        input.pPrepRenderer,
        input.frameResourceBudget,
        input.lastCameraPosition,
        cameraCart.longitude(),
        cameraCart.latitude(),
        input.contentAccess,
        input.performanceTimings,
        binding.occlusionUserData,
        binding.checkOcclusion,
        binding.onVisitTile,
        binding.onVisitTileUserData,
        {},
        input.rasterFrame};
}

} // namespace earth_engine
