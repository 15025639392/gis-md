#pragma once

#include "TileContentUpsampleKind.h"
#include "TileLoadState.h"

namespace earth_engine {

enum class TileLoadRequestKind {
    Skip,
    TerrainContentUpsample,
    Content
};

struct TileLoadRequestSnapshot {
    bool hasTile = false;
    TileContentUpsampleKind upsampleKind = TileContentUpsampleKind::None;
    bool contentProviderSupportsTile = false;
    bool contentProviderOwnsTerrainQuadtree = false;
    bool hasRenderContent = false;
    TileLoadState loadState = TileLoadState::Unloaded;
};

class TileLoadRequestPlanner {
public:
    static TileLoadRequestKind classify(
        const TileLoadRequestSnapshot& snapshot);
};

} // namespace earth_engine
