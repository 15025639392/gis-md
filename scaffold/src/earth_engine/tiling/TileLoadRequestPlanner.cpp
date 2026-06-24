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
        return TileLoadRequestKind::TerrainContentUpsample;
    }

    if (snapshot.contentProviderSupportsTile) {
        if (snapshot.hasRenderContent &&
            snapshot.loadState != TileLoadState::FailedTemporarily) {
            return TileLoadRequestKind::Skip;
        }
        return TileLoadRequestKind::Content;
    }

    if (snapshot.contentProviderOwnsTerrainQuadtree) {
        return TileLoadRequestKind::Skip;
    }

    return TileLoadRequestKind::Skip;
}

} // namespace earth_engine
