#include "TileTerrainHeightRangePolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TileBoundsMetrics.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileTerrainHeightRangePolicy::setTerrainHeightRange(
    TilesetTile& tile,
    double minimumHeight,
    double maximumHeight) {
    tile.content.renderContent.setTerrainHeightRange(minimumHeight, maximumHeight);
}

void TileTerrainHeightRangePolicy::setDefaultTerrainHeightRange(
    TilesetTile& tile) {
    setTerrainHeightRange(
        tile,
        TileBoundsMetrics::kDefaultTerrainMinimumHeight,
        TileBoundsMetrics::kDefaultTerrainMaximumHeight);
}

void TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
    TilesetTile& child,
    const TilesetTile& parent) {
    if (parent.content.renderContent.hasTerrainHeightRange()) {
        setTerrainHeightRange(
            child,
            parent.content.renderContent.terrainMinimumHeight(),
            parent.content.renderContent.terrainMaximumHeight());
    } else {
        setDefaultTerrainHeightRange(child);
    }
}

void TileTerrainHeightRangePolicy::applyMeshOrHeightmapRange(
    TilesetTile& tile,
    const SurfaceTileMesh* mesh,
    const DecodedHeightmap* heightmap) {
    if (mesh && mesh->hasHeightRange) {
        setTerrainHeightRange(tile, mesh->minimumHeight, mesh->maximumHeight);
    } else if (heightmap && heightmap->valid()) {
        setTerrainHeightRange(tile, heightmap->minHeight, heightmap->maxHeight);
    } else {
        setDefaultTerrainHeightRange(tile);
    }
}

void TileTerrainHeightRangePolicy::inheritHeightRangeForUnreadyChildren(
    TilesetTile& parent) {
    for (TilesetTile* child : parent.children) {
        if (child && !child->renderableSnapshot(true).meshReady) {
            inheritTerrainHeightRange(*child, parent);
        }
    }
}

} // namespace earth_engine
