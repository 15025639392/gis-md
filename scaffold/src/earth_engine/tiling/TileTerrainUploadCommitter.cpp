#include "TileTerrainUploadCommitter.h"

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
    if (!terrainProvider ||
        content.quantizedMeshAvailabilityUpdates.empty()) {
        return;
    }

    terrainProvider->applyAvailabilityUpdates(
        content.quantizedMeshAvailabilityUpdates);
}

void TileTerrainUploadCommitter::prepareTerrainRenderContent(
    TilesetTile& tile,
    TileLoadedContent&& content,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device) {
    if (content.hasGltfTerrainPayload()) {
        tile.content.renderContent.prepareGltfContent(
            std::move(content.gltfModel),
            content.contentTransform);
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
        return;
    }

    if (content.terrainPayloadKind == TerrainTilePayloadKind::SurfaceMesh &&
        content.surfaceMesh &&
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
