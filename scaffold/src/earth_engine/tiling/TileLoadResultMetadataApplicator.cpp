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
}

} // namespace earth_engine
