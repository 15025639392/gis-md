#pragma once

#include "TileFrameState.h"
#include "TileSubtreeTraversal.h"
#include "TileUnloadPolicy.h"
#include "TilesetTile.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace earth_engine {

class IPrepareRendererResources;

class TileSubtreeRemovalCoordinator {
public:
    template <typename CacheKeyForTileFn, typename EraseTileIndexStateFn>
    static bool clearChildrenRecursively(
        TilesetTile* tile,
        std::unordered_map<std::string, std::unique_ptr<TilesetTile>>& tiles,
        IPrepareRendererResources* pPrepRenderer,
        CacheKeyForTileFn&& cacheKeyForTile,
        EraseTileIndexStateFn&& eraseTileIndexState) {
        if (!tile) {
            return false;
        }

        const std::vector<TileSubtreeRemovalEntry> descendants =
            TileSubtreeTraversal::collectDescendantsForRemoval(
                *tile,
                cacheKeyForTile);

        for (const TileSubtreeRemovalEntry& descendant : descendants) {
            if (descendant.tile &&
                descendant.tile->referenceCount() > 0) {
                return false;
            }
        }

        for (const TileSubtreeRemovalEntry& descendant : descendants) {
            if (!descendant.tile) {
                continue;
            }
            TileUnloadPolicy::releaseRasterOverlayReferences(
                *descendant.tile,
                pPrepRenderer);
        }

        for (auto it = descendants.rbegin(); it != descendants.rend(); ++it) {
            TilesetTile* child = it->tile;
            if (!child) {
                continue;
            }
            child->parent = nullptr;
            eraseTileIndexState(it->cacheKey);
            tiles.erase(it->cacheKey);
        }
        tile->clearChildren();
        return true;
    }

    static void detachInactiveRasterOverlays(
        const std::vector<TileFrameInactiveEntry>& inactiveTiles,
        IPrepareRendererResources* pPrepRenderer) {
        for (const TileFrameInactiveEntry& entry : inactiveTiles) {
            if (!entry.tile) {
                continue;
            }
            TileUnloadPolicy::releaseRasterOverlayReferences(
                *entry.tile,
                pPrepRenderer);
        }
    }
};

} // namespace earth_engine
