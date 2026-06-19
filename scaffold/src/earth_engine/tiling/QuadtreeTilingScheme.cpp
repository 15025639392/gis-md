#include "QuadtreeTilingScheme.h"

#include <utility>

namespace earth_engine {

QuadtreeTilingScheme::QuadtreeTilingScheme(Rectangle rectangle,
                                           int rootTilesX,
                                           int rootTilesY,
                                           std::string schemeId)
    : rectangle_(rectangle),
      rootTilesX_(rootTilesX),
      rootTilesY_(rootTilesY),
      schemeId_(std::move(schemeId)) {}

int QuadtreeTilingScheme::tileCountX(int level) const {
    return rootTilesX_ << level;
}

int QuadtreeTilingScheme::tileCountY(int level) const {
    return rootTilesY_ << level;
}

std::optional<TileKey>
QuadtreeTilingScheme::positionToTile(double x, double y, int level) const {
    if (!rectangle_.contains(x, y)) {
        return std::nullopt;
    }

    const int xTiles = tileCountX(level);
    const int yTiles = tileCountY(level);
    const double xTileWidth = rectangle_.width() / static_cast<double>(xTiles);
    const double yTileHeight = rectangle_.height() / static_cast<double>(yTiles);

    int xTile = static_cast<int>((x - rectangle_.west()) / xTileWidth);
    if (xTile >= xTiles) {
        xTile = xTiles - 1;
    }

    int yTile = static_cast<int>((y - rectangle_.south()) / yTileHeight);
    if (yTile >= yTiles) {
        yTile = yTiles - 1;
    }

    return TileKey{schemeId_, level, xTile, yTile};
}

Rectangle QuadtreeTilingScheme::tileToRectangle(const TileKey& tile) const {
    const int xTiles = tileCountX(tile.z);
    const int yTiles = tileCountY(tile.z);

    const double xTileWidth = rectangle_.width() / static_cast<double>(xTiles);
    const double left = rectangle_.west() + tile.x * xTileWidth;
    const double right = rectangle_.west() + (tile.x + 1) * xTileWidth;

    const double yTileHeight = rectangle_.height() / static_cast<double>(yTiles);
    const double bottom = rectangle_.south() + tile.y * yTileHeight;
    const double top = rectangle_.south() + (tile.y + 1) * yTileHeight;

    return Rectangle(left, bottom, right, top);
}

} // namespace earth_engine
