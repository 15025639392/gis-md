#pragma once

#include "TileChildMaterializer.h"
#include "TileKey.h"
#include "TileScheme.h"
#include "TilesetTile.h"
#include "../content/GltfContentProvider.h"

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
};

} // namespace earth_engine
