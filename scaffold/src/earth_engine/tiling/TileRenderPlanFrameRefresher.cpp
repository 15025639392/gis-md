#include "TileRenderPlanFrameRefresher.h"

#include "TileCacheKey.h"
#include "TileContentAccess.h"
#include "TileRenderPlanFinalizer.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "SurfaceTileDrawCommandBuilder.h"

namespace earth_engine {

namespace {

constexpr int kActiveInteractionRenderPrepBudget = 0;
constexpr int kRecoveryRenderPrepBudget = 1;

} // namespace

void TileRenderPlanFrameRefresher::refresh(
    TilePlan& tilePlan,
    TileContentAccess& contentAccess,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const TileRenderPlanFrameRefreshOptions& options) {
    TileRenderPlanFinalizer::refreshRenderEntries(
        tilePlan,
        TileRenderPlanFinalizeOptions{
            options.enableLodTransitionPeriod,
            options.interactionActive,
            kActiveInteractionRenderPrepBudget,
            kRecoveryRenderPrepBudget},
        [&contentAccess](const TileKey& key) {
            return contentAccess.ensureTile(key);
        },
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&rasterOverlays](const TilesetTile& tile) {
            return TileSelectionRasterOverlayPreparer::isRenderable(
                tile,
                rasterOverlays);
        },
        [&rasterOverlays](const TilesetTile& tile) {
            return SurfaceTileDrawCommandBuilder::hasDrawableBaseRaster(
                tile,
                rasterOverlays);
        });
}

} // namespace earth_engine
