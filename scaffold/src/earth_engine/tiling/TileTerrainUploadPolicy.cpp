#include "TileTerrainUploadPolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileTerrainUploadPolicy::markTerrainRenderContentLoaded(
    TilesetTile& tile) {
    tile.loadState = TileLoadState::ContentLoaded;
    tile.contentKind = TileContentKind::Render;
}

void TileTerrainUploadPolicy::markTerrainRenderContentFailedTemporarily(
    TilesetTile& tile) {
    tile.contentKind = TileContentKind::Unknown;
    tile.loadState = TileLoadState::FailedTemporarily;
}

} // namespace earth_engine
