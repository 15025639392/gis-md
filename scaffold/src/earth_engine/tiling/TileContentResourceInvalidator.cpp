#include "TileContentResourceInvalidator.h"

#include "DirectRasterMapping.h"
#include "TileCacheKey.h"
#include "TileContentCacheManager.h"
#include "TileUnloadPolicy.h"
#include "TilesetTile.h"

namespace earth_engine {

TileContentResourceInvalidator::TileContentResourceInvalidator(
    uint64_t& resourceRevision,
    TileContentCacheManager& contentCache)
    : resourceRevision_(resourceRevision),
      contentCache_(contentCache) {}

void TileContentResourceInvalidator::markResourcesChanged() {
    ++resourceRevision_;
}

void TileContentResourceInvalidator::markTileResourcesChanged(
    TilesetTile& tile) {
    ++resourceRevision_;
    reconcileTileResources(tile);
}

void TileContentResourceInvalidator::reconcileTileResources(
    TilesetTile& tile) {
    contentCache_.reconcileTileBytes(tile);
    const std::string cacheKey = TileCacheKey::forTile(tile.key);
    if (tile.referenceCount() > 0 ||
        !TileUnloadPolicy::isEligibleForContentUnloadQueue(tile)) {
        contentCache_.markIneligibleForUnloading(cacheKey);
    } else {
        contentCache_.markEligibleForUnloading(&tile, cacheKey);
    }
}

} // namespace earth_engine
