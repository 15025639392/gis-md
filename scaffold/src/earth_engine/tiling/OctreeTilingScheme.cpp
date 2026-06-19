#include "OctreeTilingScheme.h"

namespace earth_engine {

OctreeTilingScheme::OctreeTilingScheme(AxisAlignedBox box,
                                       int rootTilesX,
                                       int rootTilesY,
                                       int rootTilesZ)
    : box_(box),
      rootTilesX_(rootTilesX),
      rootTilesY_(rootTilesY),
      rootTilesZ_(rootTilesZ) {}

int OctreeTilingScheme::tileCountX(int level) const {
    return rootTilesX_ << level;
}

int OctreeTilingScheme::tileCountY(int level) const {
    return rootTilesY_ << level;
}

int OctreeTilingScheme::tileCountZ(int level) const {
    return rootTilesZ_ << level;
}

std::optional<OctreeTileID>
OctreeTilingScheme::positionToTile(const Vec3& position, int level) const {
    if (!box_.contains(position)) {
        return std::nullopt;
    }

    const int xTiles = tileCountX(level);
    const int yTiles = tileCountY(level);
    const int zTiles = tileCountZ(level);

    const double xTileSize = box_.lengthX() / static_cast<double>(xTiles);
    const double yTileSize = box_.lengthY() / static_cast<double>(yTiles);
    const double zTileSize = box_.lengthZ() / static_cast<double>(zTiles);

    int xTile = static_cast<int>((position.x() - box_.minimumX()) / xTileSize);
    if (xTile >= xTiles) {
        xTile = xTiles - 1;
    }

    int yTile = static_cast<int>((position.y() - box_.minimumY()) / yTileSize);
    if (yTile >= yTiles) {
        yTile = yTiles - 1;
    }

    int zTile = static_cast<int>((position.z() - box_.minimumZ()) / zTileSize);
    if (zTile >= zTiles) {
        zTile = zTiles - 1;
    }

    return OctreeTileID{level, xTile, yTile, zTile};
}

AxisAlignedBox OctreeTilingScheme::tileToBox(const OctreeTileID& tile) const {
    const int xTiles = tileCountX(tile.level);
    const int yTiles = tileCountY(tile.level);
    const int zTiles = tileCountZ(tile.level);

    const double xTileSize = box_.lengthX() / static_cast<double>(xTiles);
    const double yTileSize = box_.lengthY() / static_cast<double>(yTiles);
    const double zTileSize = box_.lengthZ() / static_cast<double>(zTiles);

    const double minimumX = xTileSize * tile.x;
    const double minimumY = yTileSize * tile.y;
    const double minimumZ = zTileSize * tile.z;

    return AxisAlignedBox(minimumX,
                          minimumY,
                          minimumZ,
                          minimumX + xTileSize,
                          minimumY + yTileSize,
                          minimumZ + zTileSize);
}

} // namespace earth_engine
