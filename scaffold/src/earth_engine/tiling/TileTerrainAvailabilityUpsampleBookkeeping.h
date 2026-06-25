#pragma once

#include "TileCacheKey.h"
#include "TileSubtreeTraversal.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"

#include <vector>

namespace earth_engine {

enum class TileTerrainAvailabilityUpsampleBookkeeping {
    None,
    Clear,
    Latent,
    Materialized
};

class TileTerrainAvailabilityUpsampleBookkeepingPolicy {
public:
    static void applyMaterializationResult(
        const TilesetContentProvider& contentProvider,
        const TilesetTile& tile,
        TileTerrainAvailabilityUpsampleBookkeeping bookkeeping) {
        switch (bookkeeping) {
            case TileTerrainAvailabilityUpsampleBookkeeping::None:
                break;
            case TileTerrainAvailabilityUpsampleBookkeeping::Clear:
                contentProvider.clearTerrainAvailabilityUpsampledChild(
                    tile.key);
                break;
            case TileTerrainAvailabilityUpsampleBookkeeping::Latent:
                break;
            case TileTerrainAvailabilityUpsampleBookkeeping::Materialized:
                contentProvider.noteTerrainAvailabilityUpsampledChild(
                    tile.key);
                break;
        }
    }

    static void clearRemovedSubtreeProviderState(
        const TilesetContentProvider* contentProvider,
        TilesetTile* tile) {
        if (!tile || !contentProvider ||
            !contentProvider->providesTerrainQuadtree()) {
            return;
        }

        contentProvider->clearTerrainAvailabilityUpsampledChild(tile->key);
        const std::vector<TileSubtreeRemovalEntry> descendants =
            TileSubtreeTraversal::collectDescendantsForRemoval(
                *tile,
                [](const TileKey& key) {
                    return TileCacheKey::forTile(key);
                });
        for (const TileSubtreeRemovalEntry& descendant : descendants) {
            if (descendant.tile) {
                contentProvider->clearTerrainAvailabilityUpsampledChild(
                    descendant.tile->key);
            }
        }
    }
};

} // namespace earth_engine
