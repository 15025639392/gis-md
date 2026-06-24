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
        return unloadContent(
            tile,
            cacheKey,
            &terrainCache,
            emptyContentRegistry,
            pPrepRenderer);
    }

    static TileCacheUnloadContentResult unloadContent(
        TilesetTile& tile,
        const std::string& cacheKey,
        std::unordered_map<std::string, std::unique_ptr<DecodedHeightmap>>*
            terrainCache,
        TileEmptyContentRegistry& emptyContentRegistry,
        IPrepareRendererResources* pPrepRenderer) {
        if (tile.content.loadState == TileLoadState::Unloaded) {
            return TileCacheUnloadContentResult::Remove;
        }

        if (tile.content.loadState == TileLoadState::ContentLoading) {
            return TileCacheUnloadContentResult::Keep;
        }

        TileUnloadPolicy::releaseAndClearRasterOverlayReferences(
            tile,
            pPrepRenderer);

        TileCacheUnloadContentResult result =
            TileCacheUnloadContentResult::Remove;
        switch (tile.content.contentKind) {
            case TileContentKind::External:
                if (tile.referenceCount() > 0) {
                    return TileCacheUnloadContentResult::Keep;
                }
                result = TileCacheUnloadContentResult::RemoveAndClearChildren;
                break;
            case TileContentKind::Render:
                if (tile.content.loadState != TileLoadState::Unloading &&
                    TileUnloadPolicy::hasContentLoadingUpsampledDescendant(
                        tile)) {
                    if (shouldReleaseRenderResourcesForProtectedUnload(tile)) {
                        TileUnloadPolicy::
                            releaseMainThreadRenderResourcesForProtectedUnload(
                                tile);
                    }
                    tile.markContentUnloading();
                    return TileCacheUnloadContentResult::Keep;
                }
                if (tile.content.loadState == TileLoadState::Unloading &&
                    TileUnloadPolicy::hasContentLoadingUpsampledDescendant(
                        tile)) {
                    return TileCacheUnloadContentResult::Keep;
                }
                TileUnloadPolicy::releaseRenderContentResources(tile);
                if (terrainCache) {
                    terrainCache->erase(cacheKey);
                }
                break;
            case TileContentKind::Empty:
                break;
            case TileContentKind::Unknown:
                break;
        }

        emptyContentRegistry.erase(cacheKey);
        TileUnloadPolicy::markContentUnloaded(tile);
        return result;
    }

private:
    static bool shouldReleaseRenderResourcesForProtectedUnload(
        const TilesetTile& tile) {
        return tile.content.loadState == TileLoadState::ContentLoaded ||
               tile.content.loadState == TileLoadState::Done;
    }
};

} // namespace earth_engine
