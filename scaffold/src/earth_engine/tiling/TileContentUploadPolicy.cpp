#include "TileContentUploadPolicy.h"

#include "TileLoadResultMetadataApplicator.h"
#include "DirectRasterMapping.h"
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
    // Phase 2c Stage B:保留原始高度图(供 GPU 位移建高度纹理 + 高度查询)。
    if (content.retainedHeightmap) {
        tile.content.renderContent.setRetainedHeightmap(
            std::move(content.retainedHeightmap));
    }
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
