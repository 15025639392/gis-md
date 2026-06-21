#include "TileTerrainUploadCommitter.h"

#include "RasterMappedToTilesetTile.h"
#include "TileLoadResultMetadataApplicator.h"
#include "TileTerrainUploadPolicy.h"
#include "TileLoadTypes.h"
#include "TilesetTile.h"

#include <utility>

namespace earth_engine {

void TileTerrainUploadCommitter::prepareTerrainRenderContent(
    TilesetTile& tile,
    TileLoadedContent&& content) {
    if (content.surfaceMesh &&
        !tile.content.renderContent.hasSurfaceMesh()) {
        tile.content.renderContent.setSurfaceMesh(
            std::move(content.surfaceMesh));
    }
    TileLoadResultMetadataApplicator::apply(
        tile,
        std::move(content.metadata));
    prepareTerrainRenderContent(tile);
}

void TileTerrainUploadCommitter::prepareTerrainRenderContent(
    TilesetTile& tile) {
    TileTerrainUploadPolicy::markTerrainRenderContentLoaded(tile);
}

TileTerrainUploadCommitAction
TileTerrainUploadCommitter::finishMeshResourcePreparation(
    TilesetTile& tile,
    bool resourcesReady) {
    if (!resourcesReady) {
        TileTerrainUploadPolicy::markTerrainRenderContentFailedTemporarily(
            tile);
    }
    return TileTerrainUploadCommitAction{true};
}

} // namespace earth_engine
