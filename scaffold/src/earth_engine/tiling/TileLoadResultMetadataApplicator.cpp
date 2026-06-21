#include "TileLoadResultMetadataApplicator.h"

#include "RasterMappedToTilesetTile.h"
#include "TileRenderContentState.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileLoadResultMetadataApplicator::apply(
    TilesetTile& tile,
    TileLoadResultMetadata&& metadata) {
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
