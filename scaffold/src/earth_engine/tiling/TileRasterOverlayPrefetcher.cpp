#include "TileRasterOverlayPrefetcher.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Projection.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <memory>
#include <optional>
#include <vector>

namespace earth_engine {
namespace {

std::optional<Rectangle> projectBoundingRegion(
    const TilesetTile& tile,
    RasterOverlayProjection projection) {
    if (!tile.boundingVolume ||
        tile.boundingVolume->kind != TileBoundingVolumeKind::Region) {
        return std::nullopt;
    }
    if (projection == RasterOverlayProjection::WebMercator) {
        return projectRectangleSimple(
            WebMercatorProjection(Ellipsoid::WGS84()),
            tile.boundingVolume->region);
    }
    return tile.boundingVolume->region;
}

} // namespace

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
        std::optional<Rectangle> boundingRegionRectangle =
            projectBoundingRegion(tile, projection);
        const Rectangle& rasterTargetRectangle = geometryRectangle
            ? *geometryRectangle
            : (boundingRegionRectangle ? *boundingRegionRectangle : tile.bounds);
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
            tile.boundingVolume ? &*tile.boundingVolume : nullptr,
            hasRenderContentDetails);
        mapped.loadThrottled(*activeProvider, &frameResourceBudget);
    }
}

} // namespace earth_engine
