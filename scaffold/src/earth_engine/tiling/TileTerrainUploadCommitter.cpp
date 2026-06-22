#include "TileTerrainUploadCommitter.h"

#include "../content/GltfContentProvider.h"
#include "RasterMappedToTilesetTile.h"
#include "TileContentUploadPolicy.h"
#include "TileLoadResultMetadataApplicator.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TileTerrainUploadPolicy.h"
#include "TileLoadTypes.h"
#include "TilesetTile.h"

#include <utility>

namespace earth_engine {

void TileTerrainUploadCommitter::prepareTerrainRenderContent(
    TilesetTile& tile,
    TileLoadedContent&& content,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device) {
    if (content.hasGltfTerrainPayload()) {
        TileContentUploadPolicy::prepareGltfRenderContent(
            tile,
            std::move(content));
        TileRasterOverlayDetailsGenerator::
            ensureProjectionDetailsFromActiveOverlays(
                tile,
                rasterOverlays,
                device);
        return;
    }

    tile.content.renderContent.setTerrainRenderContent(true);
    TileLoadResultMetadataApplicator::apply(
        tile,
        std::move(content.metadata));
    TileRasterOverlayDetailsGenerator::
        ensureProjectionDetailsFromActiveOverlays(
            tile,
            rasterOverlays,
            device);
    prepareTerrainRenderContent(tile);
}

void TileTerrainUploadCommitter::prepareTerrainRenderContent(
    TilesetTile& tile) {
    TileTerrainUploadPolicy::markTerrainRenderContentLoaded(tile);
}

TileTerrainUploadCommitAction
TileTerrainUploadCommitter::finishTerrainResourcePreparation(
    TilesetTile& tile,
    bool resourcesReady) {
    if (!resourcesReady) {
        TileTerrainUploadPolicy::markTerrainRenderContentFailedTemporarily(
            tile);
    }
    return TileTerrainUploadCommitAction{true};
}

} // namespace earth_engine
