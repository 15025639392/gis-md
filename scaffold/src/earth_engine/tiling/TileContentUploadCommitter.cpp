#include "TileContentUploadCommitter.h"

#include "RasterMappedToTilesetTile.h"
#include "TileContentUploadPolicy.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TilesetTile.h"

#include <utility>

namespace earth_engine {

void TileContentUploadCommitter::prepareRenderContent(
    TilesetTile& tile,
    TileLoadedContent&& content,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device) {
    const bool terrainRenderContent = content.terrainRenderContent;
    TileContentUploadPolicy::prepareGltfRenderContent(
        tile,
        std::move(content));
    if (terrainRenderContent) {
        TileRasterOverlayDetailsGenerator::
            ensureProjectionDetailsFromActiveOverlays(
                tile.content.renderContent,
                tile.boundingVolume ? &*tile.boundingVolume : nullptr,
                rasterOverlays,
                device);
    }
}

TileContentUploadCommitAction
TileContentUploadCommitter::finishRenderResourcePreparation(
    TilesetTile& tile,
    bool resourcesReady) {
    if (!resourcesReady) {
        TileContentUploadPolicy::markGltfRenderResourcesFailed(tile);
    }
    return TileContentUploadCommitAction{true};
}

} // namespace earth_engine
