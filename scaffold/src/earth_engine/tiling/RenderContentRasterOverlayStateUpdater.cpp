#include "RenderContentRasterOverlayStateUpdater.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileRasterOverlaySignature.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../renderer/Renderer.h"

#include <optional>

namespace earth_engine {
namespace {

std::optional<Rectangle> projectedBoundingVolumeRectangle(
    const TilesetTile& tile,
    RasterOverlayProjection projection) {
    return TileRasterOverlayDetailsGenerator::
        projectEffectiveContentBoundingVolumeRectangle(tile, projection);
}

} // namespace

RenderContentRasterOverlayUpdateAction
RenderContentRasterOverlayStateUpdater::update(
    Renderer& renderer,
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const std::vector<size_t>& overlayProcessingOrder,
    RenderDevice* device,
    double maximumScreenSpaceError,
    FrameResourceBudget& frameResourceBudget) {
    RenderContentRasterOverlayUpdateAction action;

    if (TileRasterOverlayReadinessPolicy::doneTileCannotHoldRasterOverlays(
            tile)) {
        tile.rasterOverlayState.releaseAndClearReferences(&renderer);
        return action;
    }

    tile.rasterOverlayState.synchronizeMappingIdentity(
        TileRasterOverlaySignature::mappingIdentity(rasterOverlays),
        &renderer);
    tile.rasterOverlayState.resizeMappingSlots(
        rasterOverlays.size(),
        &renderer);
    tile.rasterOverlayState.clearMissingProjections();
    const bool hasRenderContentDetails =
        tile.content.contentKind == TileContentKind::Render &&
        tile.content.renderContent.hasRasterOverlayDetailsContent();
    const bool mapsLoadedRenderContent =
        tile.content.contentKind == TileContentKind::Render &&
        tile.content.renderContent.hasRenderableTerrainContent();
    const bool waitForContentTerrainDetails =
        tile.waitsForContentTerrainRasterDetails();
    static const RasterOverlayDetails emptyOverlayDetails;
    const RasterOverlayDetails& overlayDetails = mapsLoadedRenderContent
        ? tile.content.renderContent.rasterOverlayDetails()
        : emptyOverlayDetails;

    std::optional<size_t> firstMoreDetailAvailable;
    std::optional<size_t> firstUnknownAvailability;
    for (size_t orderIndex = 0; orderIndex < overlayProcessingOrder.size();
         ++orderIndex) {
        const size_t i = overlayProcessingOrder[orderIndex];
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
        const std::optional<Rectangle> boundingVolumeRectangle =
            hasRenderContentDetails || waitForContentTerrainDetails
                ? std::nullopt
                : projectedBoundingVolumeRectangle(tile, projection);
        const Rectangle& rasterTargetRectangle = geometryRectangle
            ? *geometryRectangle
            : (boundingVolumeRectangle ? *boundingVolumeRectangle
                                       : tile.bounds);
        const RasterTargetScreenPixels rasterScreenPixels =
            RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
                rasterTargetRectangle,
                projection,
                tile.nonZeroGeometricError(),
                maximumScreenSpaceError);
        std::vector<RasterOverlayProjection> localMissingProjections;
        const bool mapAsRenderContent = mapsLoadedRenderContent;
        std::vector<RasterOverlayProjection>& missingProjections =
            mapAsRenderContent
                ? tile.rasterOverlayState.missingProjections()
                : localMissingProjections;
        const RasterMappedToTilesetTile::MoreDetail moreDetail =
            overlay.update(
                tile.key,
                overlayDetails,
                rasterScreenPixels.x,
                rasterScreenPixels.y,
                *activeProvider,
                &renderer,
                missingProjections,
                tile.parent,
                i,
                mapAsRenderContent,
                boundingVolumeRectangle);
        if (tile.rasterOverlayState.hasMissingProjections()) {
            action.unloadTileContent = true;
            return action;
        }
        if (moreDetail == RasterMappedToTilesetTile::MoreDetail::Yes &&
            tile.rasterOverlayState.hasReadyMapping(i) &&
            !firstMoreDetailAvailable) {
            firstMoreDetailAvailable = orderIndex;
        } else if (
            moreDetail == RasterMappedToTilesetTile::MoreDetail::Unknown &&
            !firstUnknownAvailability) {
            firstUnknownAvailability = orderIndex;
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
