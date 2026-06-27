#pragma once

#include "TileContentTerrainResiduePolicy.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

struct DecodedHeightmap;
class RenderDevice;
struct TileKey;
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
