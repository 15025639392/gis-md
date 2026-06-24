#include "TileLegacyHeightmapContentResolver.h"

#include "TileLoadStatePredicates.h"
#include "TileRefinementAvailabilityResolver.h"
#include "TileScheme.h"
#include "TilesetTile.h"
#include "../providers/TerrainProvider.h"

namespace earth_engine {

TileLegacyHeightmapContentResolver::TileLegacyHeightmapContentResolver(
    const TerrainProvider* terrainProvider,
    const TilesetContentProvider* contentProvider,
    const TileScheme& tileScheme,
    const LegacyHeightmapTerrainCache*)
    : terrainProvider_(terrainProvider),
      contentProvider_(contentProvider),
      tileScheme_(&tileScheme) {}

bool TileLegacyHeightmapContentResolver::isAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    return terrainProvider_ &&
           terrainProvider_->isAvailabilityBoundaryLevel(tile.key.z);
}

bool TileLegacyHeightmapContentResolver::canRefine(
    const TilesetTile& tile) const {
    return TileRefinementAvailabilityResolver::
        canRefineLegacyHeightmapSurfaceOrExternalContent(
        tile,
        contentProvider_,
        terrainProvider_,
        *tileScheme_,
        [this](const TilesetTile& candidate) {
            return isAvailabilityBoundaryTile(candidate);
        },
        [](const TilesetTile& candidate) {
            return TileLoadStatePredicates::
                hasResolvedAvailabilityBoundaryContent(
                    candidate.content.loadState);
        });
}

TileAvailabilityState TileLegacyHeightmapContentResolver::availabilityState(
    const TileKey& key) const {
    return terrainProvider_ ? terrainProvider_->availabilityState(key)
                            : TileAvailabilityState::NotAvailable;
}

} // namespace earth_engine
