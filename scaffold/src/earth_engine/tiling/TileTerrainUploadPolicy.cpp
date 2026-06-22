#include "TileTerrainUploadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileTerrainUploadPolicy::markTerrainRenderContentLoaded(
    TilesetTile& tile) {
    tile.markRenderContentLoaded();
}

void TileTerrainUploadPolicy::markTerrainRenderContentFailedTemporarily(
    TilesetTile& tile) {
    tile.content.renderContent.clearRenderContent();
    tile.markRenderContentFailedTemporarily();
}

} // namespace earth_engine
