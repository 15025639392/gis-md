#include "TileContentAccess.h"

#include "TileCacheKey.h"
#include "TileChildFrameMaterializer.h"
#include "TileBoundsMetrics.h"
#include "TileContentLifecycleManager.h"
#include "TileRefinementAvailabilityResolver.h"
#include "TileSelectionRootPolicy.h"
#include "TileScheme.h"
#include "TileTerrainHeightRangePolicy.h"
#include "TilesetTileRegistry.h"
#include "../content/GltfContentProvider.h"

#include <vector>
#include <algorithm>

namespace earth_engine {

namespace {

bool linkChildIfMissing(TilesetTile& parent, TilesetTile& child) {
    child.parent = &parent;
    auto& children = parent.children;
    if (std::find(children.begin(), children.end(), &child) !=
        children.end()) {
        return false;
    }
    children.push_back(&child);
    return true;
}

} // namespace

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
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(tile.key)) {
        for (const TileKey& childKey :
             TileSelectionRootPolicy::levelZeroTerrainRoots(
                 tile.key.schemeId)) {
            TilesetTile* child = ensureTile(childKey);
            if (!child) {
                continue;
            }
            if (!child->boundingVolume) {
                child->boundingVolume = TileBoundingVolume::fromRegion(
                    child->bounds,
                    TileBoundsMetrics::terrainMinimumHeight(tile),
                    TileBoundsMetrics::terrainMaximumHeight(tile));
                child->contentBoundingVolume = child->boundingVolume;
            }
            TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                *child,
                tile);
            linkChildIfMissing(tile, *child);
        }
        return;
    }

    const bool contentProviderOwnsTerrainQuadtree =
        contentProvider_ && contentProvider_->providesTerrainQuadtree();
    TileChildFrameMaterializer::ensureChildren(
        TileChildFrameMaterializeInput{
            tile,
            contentProvider_ && !contentProviderOwnsTerrainQuadtree
                ? contentProvider_->childTiles(tile.key)
                : std::vector<TileKey>{},
            tileScheme_.maxZoom(),
            hasTerrainQuadtree(),
            isAvailabilityBoundaryTile(tile) &&
                !hasResolvedAvailabilityBoundaryContent(tile)},
        [this](const TileKey& key) {
            return ensureTile(key);
        },
        [this](const TileKey& key) {
            return availabilityState(key);
        });
}

bool TileContentAccess::hasResolvedAvailabilityBoundaryContent(
    const TilesetTile& tile) const {
    return tile.content.loadState > TileLoadState::ContentLoading;
}

bool TileContentAccess::isAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    if (contentProvider_ && contentProvider_->providesTerrainQuadtree()) {
        return contentProvider_->isTerrainAvailabilityBoundaryLevel(
            tile.key.z);
    }
    return terrainProvider_ &&
           terrainProvider_->isAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::hasTerrainQuadtree() const {
    return (contentProvider_ && contentProvider_->providesTerrainQuadtree()) ||
           terrainProvider_ != nullptr;
}

bool TileContentAccess::canRefine(const TilesetTile& tile) const {
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(tile.key)) {
        return true;
    }

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
        [](const TilesetTile& candidate) {
            return candidate.content.loadState > TileLoadState::ContentLoading;
        });
}

TileAvailabilityState TileContentAccess::availabilityState(
    const TileKey& key) const {
    if (contentProvider_ && contentProvider_->providesTerrainQuadtree()) {
        return contentProvider_->terrainAvailabilityState(key);
    }
    return terrainProvider_
        ? terrainProvider_->availabilityState(key)
        : TileAvailabilityState::NotAvailable;
}

} // namespace earth_engine
