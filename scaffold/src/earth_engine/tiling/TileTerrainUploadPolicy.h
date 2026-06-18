#pragma once

namespace earth_engine {

struct TilesetTile;

struct TileTerrainUploadPolicy {
    static void markTerrainRenderContentLoaded(TilesetTile& tile);
    static void markTerrainRenderContentFailedTemporarily(TilesetTile& tile);
};

} // namespace earth_engine
