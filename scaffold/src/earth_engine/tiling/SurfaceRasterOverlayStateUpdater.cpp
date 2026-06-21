#include "SurfaceRasterOverlayStateUpdater.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../renderer/Renderer.h"

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

    tile.rasterOverlayState.resizeMappingSlots(
        rasterOverlays.size(),
        &renderer);
    tile.rasterOverlayState.clearMissingProjections();
    const RasterOverlayDetails& overlayDetails =
        tile.content.renderContent.rasterOverlayDetails();
    const bool hasRenderContentDetails =
        tile.content.contentKind == TileContentKind::Render &&
        (tile.content.renderContent.hasSurfaceMesh() ||
         tile.content.renderContent.hasGltfModel());

    std::optional<size_t> firstMoreDetailAvailable;
    std::optional<size_t> firstUnknownAvailability;
    for (size_t i : overlayProcessingOrder) {
        if (i >= tile.rasterOverlayState.mappingCount()) {
            continue;
        }
        auto* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            tile.rasterOverlayState.releaseMapping(i, &renderer);
            continue;
        }
        RasterOverlayTileProvider* activeProvider =
            activeOverlay->ensureTileProvider(device);
        if (!activeProvider) continue;
        RasterMappedToTilesetTile& overlay =
            tile.rasterOverlayState.ensureMapping(i);
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
                tile.nonZeroGeometricError(),
                maximumScreenSpaceError);
        const RasterMappedToTilesetTile::MoreDetail moreDetail =
            overlay.update(
                tile.key,
                overlayDetails,
                rasterScreenPixels.x,
                rasterScreenPixels.y,
                *activeProvider,
                &renderer,
                tile.rasterOverlayState.missingProjections(),
                tile.parent,
                i,
                tile.boundingVolume ? &*tile.boundingVolume : nullptr,
                hasRenderContentDetails);
        if (tile.rasterOverlayState.hasMissingProjections()) {
            action.unloadTileContent = true;
            return action;
        }
        if (moreDetail == RasterMappedToTilesetTile::MoreDetail::Yes &&
            tile.rasterOverlayState.hasReadyMapping(i) &&
            !firstMoreDetailAvailable) {
            firstMoreDetailAvailable = i;
        } else if (
            moreDetail == RasterMappedToTilesetTile::MoreDetail::Unknown &&
            !firstUnknownAvailability) {
            firstUnknownAvailability = i;
        }
        overlay.loadThrottled(*activeProvider, &frameResourceBudget);
    }

    action.createRasterOverlayUpsampledChildren =
        firstMoreDetailAvailable &&
        (!firstUnknownAvailability ||
         *firstUnknownAvailability > *firstMoreDetailAvailable);
    return action;
}

} // namespace earth_engine
