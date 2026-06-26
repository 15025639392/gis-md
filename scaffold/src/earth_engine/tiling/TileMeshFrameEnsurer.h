#pragma once

#include "TileContentTerrainResiduePolicy.h"

namespace earth_engine {

class IPrepareRendererResources;
struct TilesetTile;

struct TileContentTerrainMeshFrameEnsureInput {
    TilesetTile& tile;
    IPrepareRendererResources* pPrepRenderer = nullptr;
};

class TileMeshFrameEnsurer {
public:
    template <typename MarkResourcesDirtyFn>
    static void ensureContentTerrain(
        const TileContentTerrainMeshFrameEnsureInput& input,
        MarkResourcesDirtyFn&& markResourcesDirty) {
        TilesetTile& tile = input.tile;
        if (TileContentTerrainResiduePolicy::clearRejectableResidue(
                tile,
                input.pPrepRenderer)) {
            markResourcesDirty();
        }
    }
};

} // namespace earth_engine
