#pragma once

#include "../tiling/TilePlan.h"

#include <vector>

namespace earth_engine {

class Tileset;

struct ScenePrimaryTilesetRenderComposition {
    std::vector<TileRenderEntry> currentEntries;
    std::vector<TileRenderEntry> pendingEntries;
    int replacedRegionCount = 0;

    bool hasPendingCoverage() const {
        return !pendingEntries.empty();
    }
};

class ScenePrimaryTilesetRenderComposer {
public:
    static ScenePrimaryTilesetRenderComposition compose(
        const Tileset& currentPrimary,
        const Tileset& pendingPrimary);
};

} // namespace earth_engine
