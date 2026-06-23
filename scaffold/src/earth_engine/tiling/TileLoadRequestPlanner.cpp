#include "TileLoadRequestPlanner.h"

namespace earth_engine {

TileLoadRequestKind TileLoadRequestPlanner::classify(
    const TileLoadRequestSnapshot& snapshot) {
    if (snapshot.hasTile &&
        snapshot.loadState != TileLoadState::Unloaded &&
        snapshot.loadState != TileLoadState::FailedTemporarily) {
        return TileLoadRequestKind::Skip;
    }

    if (snapshot.upsampledFromParent) {
        return TileLoadRequestKind::UpsampledTerrain;
    }

    if (snapshot.contentProviderSupportsTile) {
        if (snapshot.hasRenderContent) {
            return TileLoadRequestKind::Skip;
        }
        return TileLoadRequestKind::Content;
    }

    if (snapshot.contentProviderOwnsTerrainQuadtree) {
        return TileLoadRequestKind::Skip;
    }

    if (snapshot.terrainAlreadyCached ||
        !snapshot.legacyTerrainProviderSupportsTile) {
        return TileLoadRequestKind::Skip;
    }
    return TileLoadRequestKind::LegacyTerrain;
}

} // namespace earth_engine
