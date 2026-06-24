#include "TileRasterOverlayPrefetcher.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TileRasterOverlayMappingPolicy.h"
#include "TileRasterOverlaySignature.h"
#include "TileRasterOverlayReadinessPolicy.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <memory>
#include <vector>

namespace earth_engine {

TileRasterOverlayPrefetchAction TileRasterOverlayPrefetcher::prefetch(
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const std::vector<size_t>& overlayProcessingOrder,
    RenderDevice* device,
    double maximumScreenSpaceError,
    FrameResourceBudget& frameResourceBudget,
    IPrepareRendererResources* pPrepRenderer) {
    TileRasterOverlayPrefetchAction action;
    if (TileRasterOverlayReadinessPolicy::doneTileCannotHoldRasterOverlays(
            tile)) {
        tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
        return action;
    }

    tile.rasterOverlayState.synchronizeMappingIdentity(
        TileRasterOverlaySignature::mappingIdentity(rasterOverlays),
        pPrepRenderer);
    tile.rasterOverlayState.resizeMappingSlots(
        rasterOverlays.size(),
        pPrepRenderer);
    tile.rasterOverlayState.clearMissingProjections();

    if (rasterOverlays.empty()) {
        return action;
    }

    const TileRasterOverlayMappingContext mappingContext =
        TileRasterOverlayMappingPolicy::contextFor(tile);
    const RasterOverlayDetails& overlayDetails = mappingContext.details();

    for (size_t i : overlayProcessingOrder) {
        if (i >= tile.rasterOverlayState.mappingCount()) {
            continue;
        }

        ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            tile.rasterOverlayState.releaseMapping(i, pPrepRenderer);
            continue;
        }

        RasterOverlayTileProvider* activeProvider =
            activeOverlay->ensureTileProvider(device);
        if (!activeProvider) {
            continue;
        }

        const RasterOverlayProjection projection =
            activeProvider->getProjection();
        const Rectangle* geometryRectangle =
            TileRasterOverlayMappingPolicy::geometryRectangle(
                mappingContext,
                projection);
        const std::optional<Rectangle> boundingVolumeRectangle =
            TileRasterOverlayMappingPolicy::boundingVolumeRectangle(
                tile,
                mappingContext,
                projection);
        const Rectangle& rasterTargetRectangle =
            TileRasterOverlayMappingPolicy::targetRectangle(
                tile,
                geometryRectangle,
                boundingVolumeRectangle);
        const RasterTargetScreenPixels rasterScreenPixels =
            RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
                rasterTargetRectangle,
                projection,
                tile.nonZeroGeometricError(),
                maximumScreenSpaceError);

        RasterMappedToTilesetTile& mapped =
            tile.rasterOverlayState.ensureMapping(i);
        std::vector<RasterOverlayProjection> localMissingProjections;
        const bool mapAsRenderContent =
            mappingContext.mapsLoadedRenderContent;
        std::vector<RasterOverlayProjection>& missingProjections =
            mapAsRenderContent
                ? tile.rasterOverlayState.missingProjections()
                : localMissingProjections;

        // cesium-native updates RasterMappedTo3DTile before giving any
        // throttled raster request another chance to run. Keep that order so
        // loaded/failed/stale tiles are consumed before request fanout.
        mapped.update(
            tile.key,
            overlayDetails,
            rasterScreenPixels.x,
            rasterScreenPixels.y,
            *activeProvider,
            pPrepRenderer,
            missingProjections,
            tile.parent,
            i,
            mapAsRenderContent,
            boundingVolumeRectangle);
        if (tile.rasterOverlayState.hasMissingProjections()) {
            action.unloadTileContent = true;
            return action;
        }

        RasterOverlayTile* loadingTile = mapped.getLoadingTile();
        if (loadingTile &&
            loadingTile->getState() !=
                RasterOverlayTile::LoadState::Placeholder) {
            mapped.loadThrottled(*activeProvider, &frameResourceBudget);
        }
    }
    return action;
}

} // namespace earth_engine
