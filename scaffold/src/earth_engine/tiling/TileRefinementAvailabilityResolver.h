#pragma once

#include "TileChildMaterializer.h"
#include "TileKey.h"
#include "TileScheme.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"

#include <string>
#include <unordered_map>
#include <memory>
#include <utility>
#include <vector>

namespace earth_engine {

class TileScheme;
struct DecodedHeightmap;

class TileRefinementAvailabilityResolver {
public:
    template <typename IsAvailabilityBoundaryFn,
              typename HasLoadedTerrainContentFn>
    static bool canRefineContentTerrain(
        const TilesetTile& tile,
        const TilesetContentProvider& contentProvider,
        const TileScheme& tileScheme,
        IsAvailabilityBoundaryFn&& isAvailabilityBoundary,
        HasLoadedTerrainContentFn&& hasLoadedTerrainContent) {
        return TileChildMaterializer::canRefine(
            tile,
            TileRefinementAvailabilityOptions{
                !tile.children.empty(),
                false,
                false,
                isAvailabilityBoundary(tile) && !hasLoadedTerrainContent(tile),
                true,
                false,
                tileScheme.maxZoom()},
            [](const TileKey&) {
                return std::string{};
            },
            [](const std::string&) {
                return false;
            },
            [&contentProvider](const TileKey& key) {
                return contentProvider.terrainAvailabilityState(key);
            });
    }

    template <typename CacheKeyFn, typename IsAvailabilityBoundaryFn,
              typename HasLoadedTerrainContentFn>
    static bool canRefineLegacyHeightmapSurfaceOrExternalContent(
        const TilesetTile& tile,
        const TilesetContentProvider* contentProvider,
        const TerrainProvider* legacyHeightmapTerrainProvider,
        const TileScheme& tileScheme,
        const std::unordered_map<
            std::string,
            std::unique_ptr<DecodedHeightmap>>& terrainCache,
        CacheKeyFn&& cacheKey,
        IsAvailabilityBoundaryFn&& isAvailabilityBoundary,
        HasLoadedTerrainContentFn&& hasLoadedTerrainContent) {
        const std::vector<TileKey> contentChildren =
            contentProvider
                ? contentProvider->childTiles(tile.key)
                : std::vector<TileKey>{};

        return TileChildMaterializer::canRefine(
            tile,
            TileRefinementAvailabilityOptions{
                !tile.children.empty(),
                !contentChildren.empty(),
                contentProvider &&
                    contentProvider->supportsTile(tile.key),
                isAvailabilityBoundary(tile) && !hasLoadedTerrainContent(tile),
                legacyHeightmapTerrainProvider != nullptr,
                true,
                tileScheme.maxZoom()},
            cacheKey,
            [&terrainCache](const std::string& key) {
                return terrainCache.count(key) > 0;
            },
            [legacyHeightmapTerrainProvider](const TileKey& key) {
                return legacyHeightmapTerrainProvider
                    ? legacyHeightmapTerrainProvider->availabilityState(key)
                    : TileAvailabilityState::NotAvailable;
            });
    }
};

} // namespace earth_engine
