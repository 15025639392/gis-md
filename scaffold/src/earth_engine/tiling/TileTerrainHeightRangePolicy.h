#pragma once

namespace earth_engine {

struct DecodedHeightmap;
struct SurfaceTileMesh;
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
    static void applyMeshOrHeightmapRange(
        TilesetTile& tile,
        const SurfaceTileMesh* mesh,
        const DecodedHeightmap* heightmap);
    static void inheritHeightRangeForUnreadyChildren(TilesetTile& parent);
};

} // namespace earth_engine
