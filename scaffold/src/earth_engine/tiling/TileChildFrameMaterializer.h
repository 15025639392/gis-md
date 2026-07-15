#pragma once

#include "TileChildMaterializer.h"
#include "TileLoadDomainPolicy.h"
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
    uint64_t childTopologyRevision = 0;
};

struct TileChildFrameMaterializeResult {
    bool changed = false;
    bool retryLater = false;
    bool fastPath = false;
};

class TileChildFrameMaterializer {
public:
    template <typename EnsureTileFn, typename AvailabilityStateFn>
    static TileChildFrameMaterializeResult ensureChildren(
        TileChildFrameMaterializeInput input,
        EnsureTileFn&& ensureTile,
        AvailabilityStateFn&& availabilityState) {
        const bool hasReliableTopologyVersion =
            input.childTopologyRevision != 0;
        const uint64_t configuration =
            materializationConfiguration(input);
        if (hasReliableTopologyVersion &&
            input.tile.childMaterializationStateValid &&
            input.tile.appliedChildTopologyRevision ==
                input.childTopologyRevision &&
            input.tile.appliedChildMaterializationInputRevision ==
                input.tile.childMaterializationInputRevision &&
            input.tile.appliedChildMaterializationConfiguration ==
                configuration) {
            return TileChildFrameMaterializeResult{false, false, true};
        }

        TileChildFrameMaterializeResult result;
        bool materializationComplete = true;
        if (!input.contentChildKeys.empty()) {
            result = TileChildFrameMaterializeResult{
                TileChildMaterializer::linkContentChildren(
                    input.tile,
                    input.contentChildKeys,
                    ensureTile,
                    &materializationComplete),
                false};
        } else if (!TileLoadDomainPolicy::shouldCreateTerrainChildren(
                       input.tile.key.z,
                       input.maxZoom,
                       input.hasTerrainQuadtree,
                       input.tile.content.isTerrainAvailabilityUpsample())) {
            result = {};
        } else if (input.isAvailabilityBoundaryWaitingForContent) {
            input.tile.childMaterializationStateValid = false;
            return TileChildFrameMaterializeResult{false, true, false};
        } else {
            result = TileChildFrameMaterializeResult{
                TileChildMaterializer::materializeTerrainChildren(
                    input.tile,
                    input.maxZoom,
                    availabilityState,
                    ensureTile,
                    input.contentProviderOwnsTerrainQuadtree,
                    input.pPrepRenderer,
                    &materializationComplete),
                false};
        }

        if (hasReliableTopologyVersion && materializationComplete) {
            input.tile.appliedChildTopologyRevision =
                input.childTopologyRevision;
            input.tile.appliedChildMaterializationInputRevision =
                input.tile.childMaterializationInputRevision;
            input.tile.appliedChildMaterializationConfiguration =
                configuration;
            input.tile.childMaterializationStateValid = true;
        } else {
            input.tile.childMaterializationStateValid = false;
        }
        return result;
    }

private:
    static void hashCombine(uint64_t& seed, uint64_t value) {
        seed ^= value + 0x9e3779b97f4a7c15ULL +
                (seed << 6U) + (seed >> 2U);
    }

    static uint64_t materializationConfiguration(
        const TileChildFrameMaterializeInput& input) {
        uint64_t signature = 0xcbf29ce484222325ULL;
        hashCombine(signature, static_cast<uint64_t>(input.maxZoom));
        hashCombine(signature, input.hasTerrainQuadtree ? 1U : 0U);
        hashCombine(
            signature,
            input.isAvailabilityBoundaryWaitingForContent ? 1U : 0U);
        hashCombine(
            signature,
            input.contentProviderOwnsTerrainQuadtree ? 1U : 0U);
        return signature;
    }
};

} // namespace earth_engine
