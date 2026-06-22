#include "SurfaceRasterOverlayStateUpdater.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TileRasterOverlayDetailsGenerator.h"
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
    const bool hasRenderContentDetails =
        tile.content.contentKind == TileContentKind::Render &&
        (tile.content.renderContent.hasSurfaceMesh() ||
         tile.content.renderContent.hasGltfModel());
    if (hasRenderContentDetails) {
        TileRasterOverlayDetailsGenerator::
            ensureProjectionDetailsFromActiveOverlays(
                tile.content.renderContent,
                tile.boundingVolume ? &*tile.boundingVolume : nullptr,
                rasterOverlays,
                device);
    }
    static const RasterOverlayDetails emptyOverlayDetails;
    const RasterOverlayDetails& overlayDetails = hasRenderContentDetails
        ? tile.content.renderContent.rasterOverlayDetails()
        : emptyOverlayDetails;

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
        const Rectangle* geometryRectangle = hasRenderContentDetails
            ? overlayDetails.findRectangleForOverlayProjection(projection)
            : nullptr;
        const Rectangle& rasterTargetRectangle = geometryRectangle
            ? *geometryRectangle
            : tile.bounds;
        const RasterTargetScreenPixels rasterScreenPixels =
            RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
                rasterTargetRectangle,
                projection,
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
