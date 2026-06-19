#include "TileContentResourceInvalidator.h"

#include "RasterMappedToTilesetTile.h"
#include "TileContentCacheManager.h"

namespace earth_engine {

TileContentResourceInvalidator::TileContentResourceInvalidator(
    uint64_t& resourceRevision,
    TileContentCacheManager& contentCache)
    : resourceRevision_(resourceRevision),
      contentCache_(contentCache) {}

void TileContentResourceInvalidator::markResourcesDirty() {
    ++resourceRevision_;
    contentCache_.markResourcesDirty();
}

} // namespace earth_engine
