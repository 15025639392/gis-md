#include "TileTerrainUploadCommitter.h"

#include "../providers/QuantizedMeshTerrainProvider.h"

#include "RasterMappedToTilesetTile.h"
#include "TileLoadResultMetadataApplicator.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TileTerrainUploadPolicy.h"
#include "TileLoadTypes.h"
#include "TilesetTile.h"

#include <utility>

namespace earth_engine {

void TileTerrainUploadCommitter::applyAvailabilityUpdates(
    TerrainProvider* terrainProvider,
    const TileLoadedContent& content) {
    if (content.quantizedMeshAvailabilityUpdates.empty()) {
        return;
    }

    auto* qmProvider =
        dynamic_cast<QuantizedMeshTerrainProvider*>(terrainProvider);
    if (!qmProvider) {
        return;
    }

    qmProvider->applyAvailabilityUpdates(
        content.quantizedMeshAvailabilityUpdates);
}

void TileTerrainUploadCommitter::prepareTerrainRenderContent(
    TilesetTile& tile,
    TileLoadedContent&& content,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device) {
    if (content.surfaceMesh &&
        !tile.content.renderContent.hasSurfaceMesh()) {
        tile.content.renderContent.setSurfaceMesh(
            std::move(content.surfaceMesh));
    }
    TileLoadResultMetadataApplicator::apply(
        tile,
        std::move(content.metadata));
    TileRasterOverlayDetailsGenerator::
        ensureProjectionDetailsFromActiveOverlays(
            tile.content.renderContent,
            tile.boundingVolume ? &*tile.boundingVolume : nullptr,
            rasterOverlays,
            device);
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
