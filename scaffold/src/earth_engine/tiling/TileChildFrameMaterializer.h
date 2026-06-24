#pragma once

#include "TileChildMaterializer.h"
#include "TilesetTile.h"

#include <vector>

namespace earth_engine {

class IPrepareRendererResources;

struct TileChildFrameMaterializeInput {
    TilesetTile& tile;
    std::vector<TileKey> contentChildKeys;
    int maxZoom = 0;
    bool hasTerrainQuadtree = false;
    bool isAvailabilityBoundaryWaitingForContent = false;
    bool contentProviderOwnsTerrainQuadtree = false;
    IPrepareRendererResources* pPrepRenderer = nullptr;
};

struct TileChildFrameMaterializeResult {
    bool changed = false;
    bool retryLater = false;
};

class TileChildFrameMaterializer {
public:
    template <typename EnsureTileFn, typename AvailabilityStateFn>
    static TileChildFrameMaterializeResult ensureChildren(
        TileChildFrameMaterializeInput input,
        EnsureTileFn&& ensureTile,
        AvailabilityStateFn&& availabilityState) {
        if (!input.contentChildKeys.empty()) {
            return TileChildFrameMaterializeResult{
                TileChildMaterializer::linkContentChildren(
                    input.tile,
                    input.contentChildKeys,
                    ensureTile),
                false};
        }

        if (input.tile.key.z >= input.maxZoom) {
            return {};
        }
        if (input.tile.content.isTerrainAvailabilityUpsample()) {
            return {};
        }
        if (!input.hasTerrainQuadtree) {
            return {};
        }
        if (input.isAvailabilityBoundaryWaitingForContent) {
            return TileChildFrameMaterializeResult{false, true};
        }

        return TileChildFrameMaterializeResult{
            TileChildMaterializer::materializeTerrainChildren(
                input.tile,
                input.maxZoom,
                availabilityState,
                ensureTile,
                input.contentProviderOwnsTerrainQuadtree,
                input.pPrepRenderer),
            false};
    }
};

} // namespace earth_engine
