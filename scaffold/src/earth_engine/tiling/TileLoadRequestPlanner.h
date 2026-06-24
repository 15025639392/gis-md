#pragma once

#include "TileLoadState.h"

namespace earth_engine {

enum class TileLoadRequestKind {
    Skip,
    UpsampledTerrain,
    TerrainContentUpsample,
    Content
};

struct TileLoadRequestSnapshot {
    bool hasTile = false;
    bool upsampledFromParent = false;
    bool heightmapSurfacePathEnabled = false;
    bool contentProviderSupportsTile = false;
    bool contentProviderOwnsTerrainQuadtree = false;
    bool terrainAlreadyCached = false;
    bool hasRenderContent = false;
    TileLoadState loadState = TileLoadState::Unloaded;
};

class TileLoadRequestPlanner {
public:
    static TileLoadRequestKind classify(
        const TileLoadRequestSnapshot& snapshot);
};

} // namespace earth_engine
