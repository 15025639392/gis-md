#pragma once

namespace earth_engine {

struct TilesetTile;

struct TileTerrainHeightRangePolicy {
    static void setTerrainHeightRange(
        TilesetTile& tile,
        double minimumHeight,
        double maximumHeight);
    static void setDefaultTerrainHeightRange(TilesetTile& tile);
    static void inheritTerrainHeightRange(
        TilesetTile& child,
        const TilesetTile& parent);
    static void inheritHeightRangeForUnreadyChildren(TilesetTile& parent);
};

} // namespace earth_engine
