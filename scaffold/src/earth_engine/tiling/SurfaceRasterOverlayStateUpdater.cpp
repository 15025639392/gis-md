#include "SurfaceRasterOverlayStateUpdater.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../renderer/Renderer.h"

#include <memory>
#include <optional>

namespace earth_engine {

SurfaceRasterOverlayUpdateAction SurfaceRasterOverlayStateUpdater::update(
    Renderer& renderer,
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const std::vector<size_t>& overlayProcessingOrder,
    RenderDevice* device,
    double maximumScreenSpaceError,
    FrameResourceBudget& frameResourceBudget) {
    SurfaceRasterOverlayUpdateAction action;

    if (tile.rasterOverlays.size() < rasterOverlays.size()) {
        tile.rasterOverlays.resize(rasterOverlays.size());
    }
    tile.missingRasterOverlayProjections.clear();
    const RasterOverlayDetails& overlayDetails =
        tile.mesh ? tile.mesh->rasterOverlayDetails : RasterOverlayDetails{};

    std::optional<size_t> firstMoreDetailAvailable;
    std::optional<size_t> firstUnknownAvailability;
    for (size_t i : overlayProcessingOrder) {
        if (i >= tile.rasterOverlays.size()) {
            continue;
        }
        auto* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        RasterOverlayTileProvider* activeProvider =
            activeOverlay->ensureTileProvider(device);
        if (!activeProvider) continue;
        auto& overlay = tile.rasterOverlays[i];
        if (!overlay) {
            overlay = std::make_unique<RasterMappedToTilesetTile>();
        }
        const RasterOverlayProjection projection =
            activeProvider->getProjection();
        const Rectangle* geometryRectangle =
            overlayDetails.findRectangleForOverlayProjection(projection);
        std::optional<Rectangle> boundingRegionRectangle;
        if (!geometryRectangle &&
            tile.boundingVolume &&
            tile.boundingVolume->kind == TileBoundingVolumeKind::Region) {
            boundingRegionRectangle = tile.boundingVolume->region;
        }
        const Rectangle& rasterTargetRectangle = geometryRectangle
            ? *geometryRectangle
            : (boundingRegionRectangle ? *boundingRegionRectangle : tile.bounds);
        const RasterTargetScreenPixels rasterScreenPixels =
            RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
                rasterTargetRectangle,
                tile.geometricError,
                maximumScreenSpaceError);
        const RasterMappedToTilesetTile::MoreDetail moreDetail =
            overlay->update(
                tile.key,
                overlayDetails,
                rasterScreenPixels.x,
                rasterScreenPixels.y,
                *activeProvider,
                &renderer,
                tile.missingRasterOverlayProjections,
                tile.parent,
                i,
                tile.boundingVolume ? &*tile.boundingVolume : nullptr,
                tile.mesh != nullptr);
        if (!tile.missingRasterOverlayProjections.empty()) {
            action.unloadTileContent = true;
            return action;
        }
        if (moreDetail == RasterMappedToTilesetTile::MoreDetail::Yes &&
            !firstMoreDetailAvailable) {
            firstMoreDetailAvailable = i;
        } else if (
            moreDetail == RasterMappedToTilesetTile::MoreDetail::Unknown &&
            !firstUnknownAvailability) {
            firstUnknownAvailability = i;
        }
        overlay->loadThrottled(*activeProvider, &frameResourceBudget);
    }

    action.createRasterOverlayUpsampledChildren =
        firstMoreDetailAvailable &&
        (!firstUnknownAvailability ||
         *firstUnknownAvailability > *firstMoreDetailAvailable);
    return action;
}

} // namespace earth_engine
