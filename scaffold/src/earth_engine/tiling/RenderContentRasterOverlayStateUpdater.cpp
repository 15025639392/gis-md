#include "RenderContentRasterOverlayStateUpdater.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TileRasterOverlayMappingPolicy.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TileRasterOverlaySignature.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTileProvider.h"
#include "../renderer/Renderer.h"

namespace earth_engine {

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
    const TileRasterOverlayMappingContext mappingContext =
        TileRasterOverlayMappingPolicy::contextFor(tile);
    const RasterOverlayDetails& overlayDetails = mappingContext.details();

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
        // 闸3:目标几何缓存(computeDesiredScreenPixels 等三角开销加载时一次)。
        const TileRasterOverlayMappingPolicy::ResolvedTargetGeometry
            resolvedGeometry =
                TileRasterOverlayMappingPolicy::resolveTargetGeometry(
                    tile,
                    mappingContext,
                    projection,
                    overlay,
                    maximumScreenSpaceError);
        const std::optional<Rectangle>& boundingVolumeRectangle =
            resolvedGeometry.boundingVolumeRectangle;
        std::vector<RasterOverlayProjection> localMissingProjections;
        const bool mapAsRenderContent =
            mappingContext.mapsLoadedRenderContent;
        std::vector<RasterOverlayProjection>& missingProjections =
            mapAsRenderContent
                ? tile.rasterOverlayState.missingProjections()
                : localMissingProjections;
        const RasterMappedToTilesetTile::MoreDetail moreDetail =
            overlay.update(
                tile.key,
                overlayDetails,
                resolvedGeometry.screenPixelsX,
                resolvedGeometry.screenPixelsY,
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
        // cesium RasterOverlayCollection.cpp:234-242 records the FIRST (=minimum)
        // mapped-raster index in overlay-add order. gis traverses in priority
        // order, so track the minimum natural overlay index i explicitly instead
        // of the priority-sorted orderIndex; otherwise the doSubdivide comparison
        // below inverts when overlay priorities differ from their add order.
        if (moreDetail == RasterMappedToTilesetTile::MoreDetail::Yes &&
            tile.rasterOverlayState.hasReadyMapping(i)) {
            if (!firstMoreDetailAvailable || i < *firstMoreDetailAvailable) {
                firstMoreDetailAvailable = i;
            }
        } else if (
            moreDetail == RasterMappedToTilesetTile::MoreDetail::Unknown) {
            if (!firstUnknownAvailability || i < *firstUnknownAvailability) {
                firstUnknownAvailability = i;
            }
        }
        overlay.loadThrottled(*activeProvider, &frameResourceBudget);
    }

    action.createRasterOverlayUpsampledChildren =
        tile.content.loadState == TileLoadState::Done &&
        firstMoreDetailAvailable &&
        (!firstUnknownAvailability ||
         *firstUnknownAvailability > *firstMoreDetailAvailable);
    return action;
}

} // namespace earth_engine
