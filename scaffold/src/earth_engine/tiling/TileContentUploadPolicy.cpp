#include "TileContentUploadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileContentUploadPolicy::prepareGltfRenderContent(
    TilesetTile& tile,
    TileContentLoadResult&& result) {
    tile.content.renderContent.prepareGltfContent(
        std::move(result.gltfModel),
        result.contentTransform);
    tile.markRenderContentLoaded();
}

void TileContentUploadPolicy::markGltfRenderResourcesFailed(
    TilesetTile& tile) {
    tile.content.renderContent.clearGltfContent();
    tile.markRenderContentFailedTemporarily();
}

} // namespace earth_engine
