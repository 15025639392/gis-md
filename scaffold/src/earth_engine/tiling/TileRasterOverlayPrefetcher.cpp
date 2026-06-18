#include "TileRasterOverlayPrefetcher.h"

#include "RasterMappedToTilesetTile.h"
#include "RasterOverlayScreenSpaceMetrics.h"
#include "TilesetTile.h"

#include "../core/resources/FrameResourceBudget.h"
#include "../layers/ActivatedRasterOverlay.h"
#include "../providers/RasterOverlayTile.h"
#include "../providers/RasterOverlayTileProvider.h"

#include <memory>
#include <optional>
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

    if (tile.rasterOverlays.size() < rasterOverlays.size()) {
        tile.rasterOverlays.resize(rasterOverlays.size());
    }

    const bool hasRenderContentDetails =
        tile.contentKind == TileContentKind::Render && tile.mesh != nullptr;
    const RasterOverlayDetails* renderDetails = hasRenderContentDetails
        ? &tile.mesh->rasterOverlayDetails
        : nullptr;
    std::optional<Rectangle> boundingRegionRectangle;
    if (tile.boundingVolume &&
        tile.boundingVolume->kind == TileBoundingVolumeKind::Region) {
        boundingRegionRectangle = tile.boundingVolume->region;
    }
    const RasterOverlayDetails emptyDetails;
    const RasterOverlayDetails& overlayDetails =
        renderDetails ? *renderDetails : emptyDetails;

    for (size_t i : overlayProcessingOrder) {
        if (i >= tile.rasterOverlays.size()) {
            continue;
        }

        ActivatedRasterOverlay* activeOverlay = rasterOverlays[i];
        if (!activeOverlay || !activeOverlay->visible()) {
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
            : (boundingRegionRectangle ? *boundingRegionRectangle : tile.bounds);
        const RasterTargetScreenPixels rasterScreenPixels =
            RasterOverlayScreenSpaceMetrics::computeDesiredScreenPixels(
                rasterTargetRectangle,
                tile.geometricError,
                maximumScreenSpaceError);

        auto& mapped = tile.rasterOverlays[i];
        if (!mapped) {
            mapped = std::make_unique<RasterMappedToTilesetTile>();
        }

        RasterOverlayTile* loadingTile = mapped->getLoadingTile();
        if (loadingTile &&
            loadingTile->getState() !=
                RasterOverlayTile::LoadState::Placeholder) {
            if (loadingTile->getState() ==
                    RasterOverlayTile::LoadState::Unloaded ||
                loadingTile->getState() ==
                    RasterOverlayTile::LoadState::Loading) {
                mapped->loadThrottled(*activeProvider, &frameResourceBudget);
                continue;
            }
        }
        if (!loadingTile && mapped->getReadyTile()) {
            continue;
        }

        std::vector<RasterOverlayProjection> ignoredMissingProjections;
        mapped->update(
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
        mapped->loadThrottled(*activeProvider, &frameResourceBudget);
    }
}

} // namespace earth_engine
