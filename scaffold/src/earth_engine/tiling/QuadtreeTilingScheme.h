#pragma once

#include "TileKey.h"
#include "../core/math/Rectangle.h"

#include <optional>
#include <string>

namespace earth_engine {

class QuadtreeTilingScheme {
public:
    QuadtreeTilingScheme(Rectangle rectangle,
                         int rootTilesX,
                         int rootTilesY,
                         std::string schemeId = "");

    const Rectangle& rectangle() const { return rectangle_; }
    int rootTilesX() const { return rootTilesX_; }
    int rootTilesY() const { return rootTilesY_; }
    const std::string& schemeId() const { return schemeId_; }

    int tileCountX(int level) const;
    int tileCountY(int level) const;

    std::optional<TileKey> positionToTile(double x, double y, int level) const;
    Rectangle tileToRectangle(const TileKey& tile) const;

private:
    Rectangle rectangle_;
    int rootTilesX_ = 1;
    int rootTilesY_ = 1;
    std::string schemeId_;
};

} // namespace earth_engine
