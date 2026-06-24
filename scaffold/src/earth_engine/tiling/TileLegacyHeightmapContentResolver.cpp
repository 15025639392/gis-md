#include "TileLegacyHeightmapContentResolver.h"

#include "TileCacheKey.h"
#include "TileLoadStatePredicates.h"
#include "TileRefinementAvailabilityResolver.h"
#include "TileScheme.h"
#include "TilesetTile.h"
#include "../providers/TerrainProvider.h"

namespace earth_engine {

namespace {

const LegacyHeightmapTerrainCache& emptyLegacyHeightmapTerrainCache() {
    static const LegacyHeightmapTerrainCache empty;
    return empty;
}

} // namespace

TileLegacyHeightmapContentResolver::TileLegacyHeightmapContentResolver(
    const TerrainProvider* terrainProvider,
    const TilesetContentProvider* contentProvider,
    const TileScheme& tileScheme,
    const LegacyHeightmapTerrainCache* terrainCache)
    : terrainProvider_(terrainProvider),
      contentProvider_(contentProvider),
      tileScheme_(&tileScheme),
      terrainCache_(terrainCache) {}

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
        terrainCache_ ? *terrainCache_ : emptyLegacyHeightmapTerrainCache(),
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
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
