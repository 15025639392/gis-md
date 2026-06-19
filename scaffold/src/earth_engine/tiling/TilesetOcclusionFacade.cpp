#include "TilesetOcclusionFacade.h"

#include "TileOcclusionResolver.h"
#include "TileSoftwareOcclusionPolicy.h"
#include "Tileset.h"

#include <utility>

namespace earth_engine {

void TilesetOcclusionFacade::setOcclusionCallback(
    Tileset& tileset,
    TileOcclusionCallback callback) {
    tileset.occlusionCallback_ = std::move(callback);
}

void TilesetOcclusionFacade::clearOcclusionCallback(Tileset& tileset) {
    tileset.occlusionCallback_ = nullptr;
}

TileOcclusionState TilesetOcclusionFacade::checkSingleTileOcclusion(
    const Tileset& tileset,
    const TilesetTile& tile) {
    if (tileset.occlusionCallback_) {
        return tileset.occlusionCallback_(tile);
    }
    return TileSoftwareOcclusionPolicy::check(tile, tileset.lastCameraPosition_);
}

TileOcclusionState TilesetOcclusionFacade::checkOcclusion(
    const Tileset& tileset,
    const TilesetTile& tile) {
    return TileOcclusionResolver::check(
        tile,
        [&tileset](const TilesetTile& occlusionTile) {
            return checkSingleTileOcclusion(tileset, occlusionTile);
        });
}

} // namespace earth_engine
