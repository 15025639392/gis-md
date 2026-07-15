#include "TileTerrainHeightRangePolicy.h"

#include "RasterMappedToTilesetTile.h"
#include "TileBoundsMetrics.h"
#include "TilesetTile.h"

namespace earth_engine {

void TileTerrainHeightRangePolicy::setTerrainHeightRange(
    TilesetTile& tile,
    double minimumHeight,
    double maximumHeight) {
    if (tile.content.renderContent.hasTerrainHeightRange() &&
        tile.content.renderContent.terrainMinimumHeight() == minimumHeight &&
        tile.content.renderContent.terrainMaximumHeight() == maximumHeight) {
        return;
    }
    tile.content.renderContent.setTerrainHeightRange(minimumHeight, maximumHeight);
    tile.notifyChildMaterializationStateChanged();
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

void TileTerrainHeightRangePolicy::inheritHeightRangeForUnreadyChildren(
    TilesetTile& parent) {
    for (TilesetTile* child : parent.children) {
        if (child && !child->renderableSnapshot(true).meshReady) {
            inheritTerrainHeightRange(*child, parent);
        }
    }
}

} // namespace earth_engine
