#include "TileContentUploadPolicy.h"

#include "TileLoadResultMetadataApplicator.h"
#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileContentUploadPolicy::prepareGltfRenderContent(
    TilesetTile& tile,
    TileLoadedContent&& content) {
    tile.content.renderContent.prepareGltfContent(
        std::move(content.gltfModel),
        content.contentTransform);
    tile.content.renderContent.setTerrainRenderContent(false);
    TileLoadResultMetadataApplicator::apply(
        tile,
        std::move(content.metadata));
    tile.markRenderContentLoaded();
}

void TileContentUploadPolicy::markGltfRenderResourcesFailed(
    TilesetTile& tile) {
    tile.content.renderContent.clearGltfContent();
    tile.markRenderContentFailedTemporarily();
}

} // namespace earth_engine
