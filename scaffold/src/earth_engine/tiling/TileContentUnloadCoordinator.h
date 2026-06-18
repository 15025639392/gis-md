#pragma once

#include "TileCacheUnloadCoordinator.h"
#include "TileEmptyContentRegistry.h"
#include "TileUnloadPolicy.h"
#include "TilesetTile.h"
#include "../terrain/TerrainTile.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace earth_engine {

class IPrepareRendererResources;
struct DecodedHeightmap;

class TileContentUnloadCoordinator {
public:
    static TileCacheUnloadContentResult unloadContent(
        TilesetTile& tile,
        const std::string& cacheKey,
        std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>&
            terrainCache,
        TileEmptyContentRegistry& emptyContentRegistry,
        IPrepareRendererResources* pPrepRenderer) {
        if (tile.loadState == TileLoadState::Unloaded) {
            return TileCacheUnloadContentResult::Remove;
        }

        if (tile.loadState == TileLoadState::ContentLoading) {
            return TileCacheUnloadContentResult::Keep;
        }

        TileUnloadPolicy::releaseAndClearRasterOverlayReferences(
            tile,
            pPrepRenderer);

        TileCacheUnloadContentResult result =
            TileCacheUnloadContentResult::Remove;
        switch (tile.contentKind) {
            case TileContentKind::External:
                result = TileCacheUnloadContentResult::RemoveAndClearChildren;
                break;
            case TileContentKind::Render:
                if (tile.loadState != TileLoadState::Unloading &&
                    TileUnloadPolicy::hasContentLoadingUpsampledDescendant(
                        tile)) {
                    TileUnloadPolicy::
                        releaseMainThreadRenderResourcesForProtectedUnload(
                            tile);
                    tile.loadState = TileLoadState::Unloading;
                    return TileCacheUnloadContentResult::Keep;
                }
                if (tile.loadState == TileLoadState::Unloading &&
                    TileUnloadPolicy::hasContentLoadingUpsampledDescendant(
                        tile)) {
                    return TileCacheUnloadContentResult::Keep;
                }
                TileUnloadPolicy::releaseRenderContentResources(tile);
                terrainCache.erase(cacheKey);
                break;
            case TileContentKind::Empty:
                emptyContentRegistry.erase(cacheKey);
                break;
            case TileContentKind::Unknown:
                break;
        }

        TileUnloadPolicy::markContentUnloaded(tile);
        return result;
    }
};

} // namespace earth_engine
