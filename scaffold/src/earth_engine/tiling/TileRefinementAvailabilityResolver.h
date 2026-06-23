#pragma once

#include "LegacyHeightmapTerrainCacheMode.h"
#include "TileChildMaterializer.h"
#include "TileKey.h"
#include "TileScheme.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

namespace earth_engine {

class TileScheme;
struct DecodedHeightmap;

class TileRefinementAvailabilityResolver {
public:
    template <typename CacheKeyFn, typename IsAvailabilityBoundaryFn,
              typename HasLoadedTerrainContentFn>
    static bool canRefine(
        const TilesetTile& tile,
        const TilesetContentProvider* contentProvider,
        const TerrainProvider* legacyTerrainProvider,
        const TileScheme& tileScheme,
        const std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        LegacyHeightmapTerrainCacheMode legacyHeightmapCacheMode,
        CacheKeyFn&& cacheKey,
        IsAvailabilityBoundaryFn&& isAvailabilityBoundary,
        HasLoadedTerrainContentFn&& hasLoadedTerrainContent) {
        const bool contentProviderOwnsTerrainQuadtree =
            contentProvider && contentProvider->providesTerrainQuadtree();
        const TerrainProvider* effectiveLegacyTerrainProvider =
            contentProviderOwnsTerrainQuadtree ? nullptr : legacyTerrainProvider;
        const std::vector<TileKey> contentChildren =
            contentProvider && !contentProviderOwnsTerrainQuadtree
                ? contentProvider->childTiles(tile.key)
                : std::vector<TileKey>{};
        const bool legacyHeightmapCacheCanRefine =
            legacyHeightmapCacheMode == LegacyHeightmapTerrainCacheMode::Include;

        return TileChildMaterializer::canRefine(
            tile,
            TileRefinementAvailabilityOptions{
                !tile.children.empty(),
                !contentChildren.empty(),
                contentProvider &&
                    !contentProvider->providesTerrainQuadtree() &&
                    contentProvider->supportsTile(tile.key),
                isAvailabilityBoundary(tile) && !hasLoadedTerrainContent(tile),
                contentProviderOwnsTerrainQuadtree ||
                    effectiveLegacyTerrainProvider != nullptr,
                legacyHeightmapCacheCanRefine,
                tileScheme.maxZoom()},
            cacheKey,
            [legacyHeightmapCacheCanRefine, &terrainCache](
                const std::string& key) {
                if (!legacyHeightmapCacheCanRefine) {
                    return false;
                }
                return terrainCache.count(key) > 0;
            },
            [contentProvider,
             effectiveLegacyTerrainProvider](const TileKey& key) {
                if (contentProvider &&
                    contentProvider->providesTerrainQuadtree()) {
                    return contentProvider->terrainAvailabilityState(key);
                }
                return effectiveLegacyTerrainProvider
                    ? effectiveLegacyTerrainProvider->availabilityState(key)
                    : TileAvailabilityState::NotAvailable;
            });
    }
};

} // namespace earth_engine
