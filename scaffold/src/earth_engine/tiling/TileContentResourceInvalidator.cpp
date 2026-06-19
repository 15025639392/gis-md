#include "TileContentResourceInvalidator.h"

#include "RasterMappedToTilesetTile.h"
#include "TileContentCacheManager.h"
#include "TileSelectionReuseState.h"

namespace earth_engine {

TileContentResourceInvalidator::TileContentResourceInvalidator(
    uint64_t& resourceRevision,
    TileContentCacheManager& contentCache,
    TileSelectionReuseState& selectionReuseState)
    : resourceRevision_(resourceRevision),
      contentCache_(contentCache),
      selectionReuseState_(selectionReuseState) {}

void TileContentResourceInvalidator::markResourcesDirty() {
    ++resourceRevision_;
    contentCache_.markResourcesDirty();
    selectionReuseState_.invalidate();
}

} // namespace earth_engine
