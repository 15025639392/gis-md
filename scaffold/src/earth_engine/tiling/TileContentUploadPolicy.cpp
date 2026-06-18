#include "TileContentUploadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileContentUploadPolicy::prepareGltfRenderContent(
    TilesetTile& tile,
    TileContentLoadResult&& result) {
    tile.heightmap.reset();
    tile.mesh.reset();
    tile.gpuVertexBuffer.reset();
    tile.gpuIndexBuffer.reset();
    tile.gltfTextureResources.clear();
    tile.gltfPrimitiveResources.clear();
    tile.gltfModel = std::move(result.gltfModel);
    tile.gltfContentTransform = result.contentTransform;
    tile.meshReady = false;
    tile.surfaceDrawable = false;
    tile.surfaceSource = SurfaceDrawableSource::GltfContent;
    tile.contentKind = TileContentKind::Render;
    tile.loadState = TileLoadState::ContentLoaded;
}

void TileContentUploadPolicy::markGltfRenderResourcesFailed(
    TilesetTile& tile) {
    tile.gltfModel.reset();
    tile.gltfTextureResources.clear();
    tile.gltfPrimitiveResources.clear();
    tile.contentKind = TileContentKind::Unknown;
    tile.loadState = TileLoadState::FailedTemporarily;
}

} // namespace earth_engine
