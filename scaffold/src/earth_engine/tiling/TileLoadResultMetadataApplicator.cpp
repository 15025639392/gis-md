#include "TileLoadResultMetadataApplicator.h"

#include "RasterMappedToTilesetTile.h"
#include "TileRenderContentState.h"
#include "TilesetTile.h"

namespace earth_engine {
namespace {

constexpr double kLooseMinimumHeight = -1000.0;
constexpr double kLooseMaximumHeight = 9000.0;

bool isDefaultLooseRegion(const TilesetTile& tile) {
    return tile.boundingVolume &&
           tile.boundingVolume->kind == TileBoundingVolumeKind::Region &&
           tile.boundingVolume->minimumHeight == kLooseMinimumHeight &&
           tile.boundingVolume->maximumHeight == kLooseMaximumHeight;
}

bool hasValidBoundingRegion(const RasterOverlayDetails& details) {
    return !details.boundingRegion.rectangle.isEmpty() &&
           details.boundingRegion.minimumHeight <=
               details.boundingRegion.maximumHeight;
}

} // namespace

void TileLoadResultMetadataApplicator::apply(
    TilesetTile& tile,
    TileLoadResultMetadata&& metadata) {
    if (!metadata.updatedBoundingVolume &&
        metadata.rasterOverlayDetails &&
        isDefaultLooseRegion(tile) &&
        hasValidBoundingRegion(*metadata.rasterOverlayDetails)) {
        const auto& region = metadata.rasterOverlayDetails->boundingRegion;
        metadata.updatedBoundingVolume = TileBoundingVolume::fromRegion(
            region.rectangle,
            region.minimumHeight,
            region.maximumHeight);
    }

    if (metadata.updatedBoundingVolume) {
        tile.boundingVolume = std::move(metadata.updatedBoundingVolume);
        if (!metadata.terrainHeightRange) {
            metadata.terrainHeightRange = {
                tile.boundingVolume->minimumHeight,
                tile.boundingVolume->maximumHeight};
        }
    }
    if (metadata.updatedContentBoundingVolume) {
        tile.contentBoundingVolume =
            std::move(metadata.updatedContentBoundingVolume);
    }
    if (metadata.rasterOverlayDetails) {
        if (RasterOverlayDetails* details =
                tile.content.renderContent.mutableRasterOverlayDetails()) {
            *details = std::move(*metadata.rasterOverlayDetails);
        }
    }
    if (metadata.terrainHeightRange) {
        tile.content.renderContent.setTerrainHeightRange(
            metadata.terrainHeightRange->first,
            metadata.terrainHeightRange->second);
    }
    if (metadata.horizonOcclusionPoint) {
        tile.content.renderContent.setHorizonOcclusionPoint(
            *metadata.horizonOcclusionPoint);
    }
}

} // namespace earth_engine
