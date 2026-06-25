#include "TileContentUploadCommitter.h"

#include "RasterMappedToTilesetTile.h"
#include "TileContentUploadPolicy.h"
#include "TileRasterOverlayDetailsGenerator.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"

#include <optional>
#include <utility>

namespace earth_engine {
namespace {

std::optional<TileBoundingVolume> effectiveContentBoundingVolumeForLoad(
    const TilesetTile& tile,
    const TileLoadedContent& content) {
    if (content.metadata.updatedContentBoundingVolume) {
        return *content.metadata.updatedContentBoundingVolume;
    }
    if (content.metadata.updatedBoundingVolume) {
        return *content.metadata.updatedBoundingVolume;
    }
    if (tile.contentBoundingVolume) {
        return *tile.contentBoundingVolume;
    }
    if (tile.boundingVolume) {
        return *tile.boundingVolume;
    }
    return std::nullopt;
}

void restoreInitialBoundingVolumesAfterResourceFailure(TilesetTile& tile) {
    if (tile.initialBoundingVolume) {
        tile.boundingVolume = tile.initialBoundingVolume;
    }
    if (tile.initialContentBoundingVolume) {
        tile.contentBoundingVolume = tile.initialContentBoundingVolume;
    } else {
        tile.contentBoundingVolume.reset();
    }
}

} // namespace

void TileContentUploadCommitter::prepareRenderContent(
    TilesetTile& tile,
    TileLoadedContent&& content,
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays,
    RenderDevice* device,
    IPrepareRendererResources* pPrepRenderer,
    TilesetContentProvider* contentProvider) {
    const std::optional<TileBoundingVolume> effectiveContentBoundingVolume =
        effectiveContentBoundingVolumeForLoad(tile, content);
    if (contentProvider &&
        contentProvider->providesTerrainQuadtree() &&
        content.satisfiesContentTerrainPayloadContract() &&
        !content.quantizedMeshAvailabilityUpdates.empty()) {
        contentProvider->applyTerrainAvailabilityUpdates(
            content.quantizedMeshAvailabilityUpdates);
    }
    // cesium-native RasterOverlayCollection::addTileOverlays clears old
    // mappings before a new render-content generation maps overlays.
    tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
    TileContentUploadPolicy::prepareGltfRenderContent(
        tile,
        std::move(content));
    if (tile.content.renderContent.hasGltfModel()) {
        TileRasterOverlayDetailsGenerator::
            ensureProjectionDetailsFromActiveOverlays(
                tile.content.renderContent,
                effectiveContentBoundingVolume
                    ? &*effectiveContentBoundingVolume
                    : nullptr,
                rasterOverlays,
                device);
    }
}

TileContentUploadCommitAction
TileContentUploadCommitter::finishRenderResourcePreparation(
    TilesetTile& tile,
    bool resourcesReady,
    IPrepareRendererResources* pPrepRenderer) {
    if (!resourcesReady) {
        restoreInitialBoundingVolumesAfterResourceFailure(tile);
        tile.rasterOverlayState.releaseAndClearReferences(pPrepRenderer);
        TileContentUploadPolicy::markGltfRenderResourcesFailed(tile);
    }
    return TileContentUploadCommitAction{resourcesReady, true};
}

} // namespace earth_engine
