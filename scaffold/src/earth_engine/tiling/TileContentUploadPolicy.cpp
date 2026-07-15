#include "TileContentUploadPolicy.h"

#include "TileLoadResultMetadataApplicator.h"
#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {
namespace {

SurfaceDrawableSource surfaceSourceForLoadedContent(
    const TileLoadedContent& content) {
    if (!content.terrainRenderContent) {
        return SurfaceDrawableSource::GltfContent;
    }
    switch (content.terrainRenderSource) {
        case TileTerrainRenderSource::EllipsoidFallback:
            return SurfaceDrawableSource::EllipsoidFallback;
        case TileTerrainRenderSource::Generic:
            return SurfaceDrawableSource::GltfContent;
    }
    return SurfaceDrawableSource::GltfContent;
}

} // namespace

void TileContentUploadPolicy::prepareGltfRenderContent(
    TilesetTile& tile,
    TileLoadedContent&& content) {
    const bool terrainRenderContent = content.terrainRenderContent;
    const SurfaceDrawableSource surfaceSource =
        surfaceSourceForLoadedContent(content);
    tile.content.renderContent.prepareGltfContent(
        std::move(content.gltfModel),
        content.contentTransform);
    tile.content.renderContent.setTerrainRenderContent(terrainRenderContent);
    tile.content.renderContent.setSurfaceSource(surfaceSource);
    TileLoadResultMetadataApplicator::apply(
        tile,
        std::move(content.metadata));
    tile.markRenderContentLoaded();
}

void TileContentUploadPolicy::markGltfRenderResourcesFailed(
    TilesetTile& tile) {
    tile.content.renderContent.clearGltfContentPreservingFill();
    tile.markRenderContentFailedTemporarily();
}

} // namespace earth_engine
