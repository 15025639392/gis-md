#pragma once

#include "TileChildMaterializer.h"
#include "TilesetTile.h"

#include <vector>

namespace earth_engine {

struct TileChildFrameMaterializeInput {
    TilesetTile& tile;
    std::vector<TileKey> contentChildKeys;
    int maxZoom = 0;
    bool hasTerrainProvider = false;
    bool isAvailabilityBoundaryWaitingForContent = false;
};

class TileChildFrameMaterializer {
public:
    template <typename EnsureTileFn, typename AvailabilityStateFn>
    static void ensureChildren(
        TileChildFrameMaterializeInput input,
        EnsureTileFn&& ensureTile,
        AvailabilityStateFn&& availabilityState) {
        if (!input.contentChildKeys.empty()) {
            TileChildMaterializer::linkContentChildren(
                input.tile,
                input.contentChildKeys,
                ensureTile);
            return;
        }

        if (input.tile.key.z >= input.maxZoom) {
            return;
        }
        if (input.tile.content.upsampledFromParent &&
            !input.tile.content.rasterUpsampledForMoreDetail) {
            return;
        }
        if (!input.hasTerrainProvider) {
            return;
        }
        if (input.isAvailabilityBoundaryWaitingForContent) {
            return;
        }

        TileChildMaterializer::materializeTerrainChildren(
            input.tile,
            input.maxZoom,
            availabilityState,
            ensureTile);
    }
};

} // namespace earth_engine
