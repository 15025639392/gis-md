#include "TileLodTransitionFrameUpdater.h"

#include "RasterMappedToTilesetTile.h"
#include "TileCacheKey.h"
#include "TileLodTransitionController.h"
#include "TileSelectionRasterOverlayPreparer.h"
#include "TilesetTileRegistry.h"

namespace earth_engine {

namespace {

bool hasLodTransitionRenderContent(
    const TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays) {
    return tile.content.contentKind == TileContentKind::Render &&
           TileSelectionRasterOverlayPreparer::isRenderable(
               tile,
               rasterOverlays);
}

} // namespace

void TileLodTransitionFrameUpdater::update(
    TilePlan& tilePlan,
    TilesetTileRegistry& tileRegistry,
    std::unordered_set<std::string>& fadingKeys,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    double deltaSeconds,
    const TileLodTransitionFrameOptions& options) {
    TileLodTransitionController::updateTransitions(
        tilePlan,
        fadingKeys,
        deltaSeconds,
        TileLodTransitionOptions{
            &tileRegistry.tiles(),
            options.enableLodTransitionPeriod,
            options.lodTransitionLength},
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [&rasterOverlays](const TilesetTile& tile) {
            return hasLodTransitionRenderContent(tile, rasterOverlays);
        });
}

} // namespace earth_engine
