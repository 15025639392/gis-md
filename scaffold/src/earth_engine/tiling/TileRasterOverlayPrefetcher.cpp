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
        // cesium-native: pass the TILESET maximumScreenSpaceError (16.0) to
        // computeDesiredScreenPixels, NOT the overlay's MSE (2.0).
        // The overlay's MSE is divided again inside
        // computeLevelFromTargetScreenPixels, creating intentional headroom
        // for higher zoom overlays.  Using overlay MSE in both places
        // underestimates the desired zoom by ~3 levels (factor 16/2 = 8).
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
            if (tile.content.loadState == TileLoadState::Done) {
                action.unloadTileContent = true;
            } else {
                tile.rasterOverlayState.releaseAndClearReferences(
                    pPrepRenderer);
            }
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

void TileRasterOverlayPrefetcher::advanceThrottledLoads(
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const std::vector<size_t>& overlayProcessingOrder,
    RenderDevice* device,
    FrameResourceBudget& frameResourceBudget) {
    for (size_t i : overlayProcessingOrder) {
        if (i >= tile.rasterOverlayState.mappingCount()) {
            continue;
        }
        ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            continue;
        }
        RasterMappedToTilesetTile* mapped =
            tile.rasterOverlayState.mappingAt(i);
        if (!mapped) {
            continue;
        }
        RasterOverlayTile* loadingTile = mapped->getLoadingTile();
        if (!loadingTile ||
            loadingTile->getState() ==
                RasterOverlayTile::LoadState::Placeholder) {
            continue;
        }
        // ensureTileProvider is cached after first creation; no geometry work.
        RasterOverlayTileProvider* activeProvider =
            activeOverlay->ensureTileProvider(device);
        if (!activeProvider) {
            continue;
        }
        mapped->loadThrottled(*activeProvider, &frameResourceBudget);
    }
}

} // namespace earth_engine
