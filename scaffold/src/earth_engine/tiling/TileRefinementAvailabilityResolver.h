#pragma once

#include "TileChildMaterializer.h"
#include "TileKey.h"
#include "TileScheme.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"
#include "../providers/TerrainProvider.h"

#include <vector>

namespace earth_engine {

class TileScheme;

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
                tileScheme.maxZoom()},
            [&contentProvider](const TileKey& key) {
                return contentProvider.availabilityState(key);
            });
    }

    template <typename IsAvailabilityBoundaryFn,
              typename HasLoadedTerrainContentFn>
    static bool canRefineLegacyHeightmapSurfaceOrExternalContent(
        const TilesetTile& tile,
        const TilesetContentProvider* contentProvider,
        const TerrainProvider* legacyHeightmapTerrainProvider,
        const TileScheme& tileScheme,
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
                tileScheme.maxZoom()},
            [legacyHeightmapTerrainProvider](const TileKey& key) {
                return legacyHeightmapTerrainProvider
                    ? legacyHeightmapTerrainProvider->availabilityState(key)
                    : TileAvailabilityState::NotAvailable;
            });
    }
};

} // namespace earth_engine
