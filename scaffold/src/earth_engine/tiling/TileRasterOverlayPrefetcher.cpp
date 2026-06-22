#include "TileRasterOverlayPrefetcher.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TileRasterOverlaySignature.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <memory>
#include <optional>
#include <vector>

namespace earth_engine {
namespace {

std::optional<Rectangle> projectedBoundingVolumeRectangle(
    const TilesetTile& tile,
    RasterOverlayProjection projection) {
    return TileRasterOverlayDetailsGenerator::
        projectEffectiveContentBoundingVolumeRectangle(tile, projection);
}

} // namespace

void TileRasterOverlayPrefetcher::prefetch(
    TilesetTile& tile,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    const std::vector<size_t>& overlayProcessingOrder,
    RenderDevice* device,
    double maximumScreenSpaceError,
    FrameResourceBudget& frameResourceBudget,
    IPrepareRendererResources* pPrepRenderer) {
    tile.rasterOverlayState.synchronizeMappingIdentity(
        TileRasterOverlaySignature::mappingIdentity(rasterOverlays),
        pPrepRenderer);
    tile.rasterOverlayState.resizeMappingSlots(
        rasterOverlays.size(),
        pPrepRenderer);
    tile.rasterOverlayState.clearMissingProjections();

    if (rasterOverlays.empty()) {
        return;
    }

    const bool hasRenderContentDetails =
        tile.content.contentKind == TileContentKind::Render &&
        tile.content.renderContent.hasRenderableTerrainContent();
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
        const Rectangle* geometryRectangle = renderDetails
            ? renderDetails->findRectangleForOverlayProjection(projection)
            : nullptr;
        const std::optional<Rectangle> boundingVolumeRectangle =
            hasRenderContentDetails
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

        std::vector<RasterOverlayProjection> localMissingProjections;
        std::vector<RasterOverlayProjection>& missingProjections =
            hasRenderContentDetails
                ? tile.rasterOverlayState.missingProjections()
                : localMissingProjections;
        mapped.update(
            tile.key,
            overlayDetails,
            rasterScreenPixels.x,
            rasterScreenPixels.y,
            *activeProvider,
            nullptr,
            missingProjections,
            tile.parent,
            i,
            hasRenderContentDetails,
            boundingVolumeRectangle);
        mapped.loadThrottled(*activeProvider, &frameResourceBudget);
    }
}

} // namespace earth_engine
