#include "TileRasterOverlayPrefetcher.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <memory>
#include <vector>

namespace earth_engine {

void TileRasterOverlayPrefetcher::prefetch(
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const std::vector<size_t>& overlayProcessingOrder,
    RenderDevice* device,
    double maximumScreenSpaceError,
    FrameResourceBudget& frameResourceBudget) {
    if (rasterOverlays.empty()) {
        return;
    }

    tile.rasterOverlayState.resizeMappingSlots(rasterOverlays.size(), nullptr);

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
    const RasterOverlayDetails* renderDetails = hasRenderContentDetails
        ? &tile.content.renderContent.rasterOverlayDetails()
        : nullptr;
    static const RasterOverlayDetails emptyOverlayDetails;
    const RasterOverlayDetails& overlayDetails = renderDetails
        ? *renderDetails
        : emptyOverlayDetails;

    for (size_t i : overlayProcessingOrder) {
        if (i >= tile.rasterOverlayState.mappingCount()) {
            continue;
        }

        ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
            tile.rasterOverlayState.releaseMapping(i, nullptr);
            continue;
        }

        RasterOverlayTileProvider* activeProvider =
            activeOverlay->ensureTileProvider(device);
        if (!activeProvider) {
            continue;
        }

        const RasterOverlayProjection projection =
            activeProvider->getProjection();
        const Rectangle* geometryRectangle = renderDetails
            ? renderDetails->findRectangleForOverlayProjection(projection)
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

        RasterMappedToTilesetTile& mapped =
            tile.rasterOverlayState.ensureMapping(i);

        RasterOverlayTile* loadingTile = mapped.getLoadingTile();
        if (loadingTile &&
            loadingTile->getState() !=
                RasterOverlayTile::LoadState::Placeholder) {
            if (loadingTile->getState() ==
                    RasterOverlayTile::LoadState::Unloaded ||
                loadingTile->getState() ==
                    RasterOverlayTile::LoadState::Loading) {
                mapped.loadThrottled(*activeProvider, &frameResourceBudget);
                continue;
            }
        }
        if (!loadingTile && mapped.getReadyTile()) {
            continue;
        }

        std::vector<RasterOverlayProjection> ignoredMissingProjections;
        mapped.update(
            tile.key,
            overlayDetails,
            rasterScreenPixels.x,
            rasterScreenPixels.y,
            *activeProvider,
            nullptr,
            ignoredMissingProjections,
            tile.parent,
            i,
            hasRenderContentDetails);
        mapped.loadThrottled(*activeProvider, &frameResourceBudget);
    }
}

} // namespace earth_engine
