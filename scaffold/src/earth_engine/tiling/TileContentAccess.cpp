#include "TileContentAccess.h"

#include "TileCacheKey.h"
#include "TileChildFrameMaterializer.h"
#include "TileContentLifecycleManager.h"
#include "TileRefinementAvailabilityResolver.h"
#include "TileScheme.h"
#include "TilesetTileRegistry.h"
#include "../content/GltfContentProvider.h"
#include "../providers/QuantizedMeshTerrainProvider.h"

#include <vector>

namespace earth_engine {

TileContentAccess::TileContentAccess(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TerrainProvider* terrainProvider,
    const TilesetContentProvider* contentProvider,
    const TileContentLifecycleManager& contentLifecycle,
    size_t rasterOverlayCount)
    : tileRegistry_(tileRegistry),
      tileScheme_(tileScheme),
      terrainProvider_(terrainProvider),
      contentProvider_(contentProvider),
      contentLifecycle_(contentLifecycle),
      rasterOverlayCount_(rasterOverlayCount) {}

TilesetTile* TileContentAccess::ensureTile(const TileKey& key) {
    return tileRegistry_.ensureTile(
        key,
        tileScheme_,
        contentProvider_,
        rasterOverlayCount_);
}

void TileContentAccess::ensureTileChildren(TilesetTile& tile) {
    TileChildFrameMaterializer::ensureChildren(
        TileChildFrameMaterializeInput{
            tile,
            contentProvider_ ? contentProvider_->childTiles(tile.key)
                             : std::vector<TileKey>{},
            tileScheme_.maxZoom(),
            terrainProvider_ != nullptr,
            isAvailabilityBoundaryTile(tile) &&
                !hasLoadedTerrainContent(tile)},
        [this](const TileKey& key) {
            return ensureTile(key);
        },
        [this](const TileKey& key) {
            return availabilityState(key);
        });
}

bool TileContentAccess::hasLoadedTerrainContent(
    const TilesetTile& tile) const {
    const auto& terrainCache = contentLifecycle_.terrainCache();
    const auto it = terrainCache.find(TileCacheKey::forTile(tile.key));
    return it != terrainCache.end() && it->second != nullptr;
}

bool TileContentAccess::isAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    const auto* qmProvider =
        dynamic_cast<const QuantizedMeshTerrainProvider*>(terrainProvider_);
    if (!qmProvider) {
        return false;
    }
    return qmProvider->isAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::canRefine(const TilesetTile& tile) const {
    return TileRefinementAvailabilityResolver::canRefine(
        tile,
        contentProvider_,
        terrainProvider_,
        tileScheme_,
        contentLifecycle_.terrainCache(),
        [](const TileKey& key) {
            return TileCacheKey::forTile(key);
        },
        [this](const TilesetTile& candidate) {
            return isAvailabilityBoundaryTile(candidate);
        },
        [this](const TilesetTile& candidate) {
            return hasLoadedTerrainContent(candidate);
        });
}

TileAvailabilityState TileContentAccess::availabilityState(
    const TileKey& key) const {
    return terrainProvider_
        ? terrainProvider_->availabilityState(key)
        : TileAvailabilityState::NotAvailable;
}

} // namespace earth_engine
