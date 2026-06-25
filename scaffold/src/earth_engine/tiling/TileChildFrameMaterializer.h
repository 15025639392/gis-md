#pragma once

#include "TileChildMaterializer.h"
#include "TileTerrainAvailabilityUpsampleBookkeeping.h"
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
    TileTerrainAvailabilityUpsampleBookkeeping terrainUpsampleBookkeeping =
        TileTerrainAvailabilityUpsampleBookkeeping::None;
};

class TileChildFrameMaterializer {
public:
    template <typename EnsureTileFn, typename AvailabilityStateFn>
    static TileChildFrameMaterializeResult ensureChildren(
        TileChildFrameMaterializeInput input,
        EnsureTileFn&& ensureTile,
        AvailabilityStateFn&& availabilityState) {
        if (!input.contentChildKeys.empty()) {
            return finalizeTerrainAvailabilityUpsampleBookkeeping(
                input,
                TileChildFrameMaterializeResult{
                TileChildMaterializer::linkContentChildren(
                    input.tile,
                    input.contentChildKeys,
                    ensureTile),
                false},
                availabilityState);
        }

        if (input.tile.key.z >= input.maxZoom) {
            return finalizeTerrainAvailabilityUpsampleBookkeeping(
                input,
                {},
                availabilityState);
        }
        if (input.tile.content.isTerrainAvailabilityUpsample()) {
            return finalizeTerrainAvailabilityUpsampleBookkeeping(
                input,
                {},
                availabilityState);
        }
        if (!input.hasTerrainQuadtree) {
            return finalizeTerrainAvailabilityUpsampleBookkeeping(
                input,
                {},
                availabilityState);
        }
        if (input.isAvailabilityBoundaryWaitingForContent) {
            return finalizeTerrainAvailabilityUpsampleBookkeeping(
                input,
                TileChildFrameMaterializeResult{false, true},
                availabilityState);
        }

        return finalizeTerrainAvailabilityUpsampleBookkeeping(
            input,
            TileChildFrameMaterializeResult{
            TileChildMaterializer::materializeTerrainChildren(
                input.tile,
                input.maxZoom,
                availabilityState,
                ensureTile,
                input.contentProviderOwnsTerrainQuadtree,
                input.pPrepRenderer),
            false},
            availabilityState);
    }

private:
    template <typename AvailabilityStateFn>
    static TileChildFrameMaterializeResult
    finalizeTerrainAvailabilityUpsampleBookkeeping(
        const TileChildFrameMaterializeInput& input,
        TileChildFrameMaterializeResult result,
        AvailabilityStateFn&& availabilityState) {
        if (!input.contentProviderOwnsTerrainQuadtree) {
            return result;
        }

        const bool hasUpsampledChild =
            TileChildMaterializer::hasTerrainAvailabilityUpsampledChild(
                input.tile,
                availabilityState);
        if (hasUpsampledChild) {
            result.terrainUpsampleBookkeeping =
                input.tile.children.empty()
                    ? TileTerrainAvailabilityUpsampleBookkeeping::Latent
                    : TileTerrainAvailabilityUpsampleBookkeeping::Materialized;
        } else {
            result.terrainUpsampleBookkeeping =
                TileTerrainAvailabilityUpsampleBookkeeping::Clear;
        }
        return result;
    }
};

} // namespace earth_engine
