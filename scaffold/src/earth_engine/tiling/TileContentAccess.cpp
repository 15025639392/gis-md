#include "TileContentAccess.h"

#include "TileCacheKey.h"
#include "TileChildFrameMaterializer.h"
#include "TileBoundsMetrics.h"
#include "TileContentTerrainResiduePolicy.h"
#include "TileContentLifecycleManager.h"
#include "TileLoadStatePredicates.h"
#include "TileRefinementAvailabilityResolver.h"
#include "TileSelectionRootPolicy.h"
#include "TileScheme.h"
#include "TileTerrainHeightRangePolicy.h"
#include "TileTerrainAvailabilityUpsampleBookkeeping.h"
#include "TilesetTileRegistry.h"
#include "../content/GltfContentProvider.h"

#include <vector>
#include <algorithm>
#include <cassert>

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

bool initializeTerrainChild(
    TilesetTile& parent,
    TilesetTile& child,
    bool clearResidue,
    IPrepareRendererResources* pPrepRenderer) {
    bool changed = false;
    if (!child.boundingVolume) {
        child.boundingVolume = TileBoundingVolume::fromLooseRegion(
            child.bounds,
            TileBoundsMetrics::terrainMinimumHeight(parent),
            TileBoundsMetrics::terrainMaximumHeight(parent));
        changed = true;
    }

    const bool hasAcceptedTerrainContent =
        TileContentTerrainResiduePolicy::hasAcceptedTerrainContent(child);
    if (!hasAcceptedTerrainContent) {
        TileTerrainHeightRangePolicy::inheritTerrainHeightRange(child, parent);
    }
    if (clearResidue &&
        TileContentTerrainResiduePolicy::clearRejectableResidue(
            child,
            pPrepRenderer)) {
        TileTerrainHeightRangePolicy::inheritTerrainHeightRange(child, parent);
        changed = true;
    }
    return changed;
}

} // namespace

TileContentAccess TileContentAccess::forContentTerrain(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TilesetContentProvider& contentProvider) {
    return TileContentAccess(
        tileRegistry,
        tileScheme,
        nullptr,
        &contentProvider,
        nullptr,
        TerrainOwnership::ContentProvider);
}

TileContentAccess TileContentAccess::forNoTerrain(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TilesetContentProvider* contentProvider) {
    return TileContentAccess(
        tileRegistry,
        tileScheme,
        nullptr,
        contentProvider,
        nullptr,
        TerrainOwnership::None);
}

TileContentAccess TileContentAccess::forHeightmapTerrainSurfacePath(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TerrainProvider* legacyHeightmapTerrainProvider,
    const TilesetContentProvider* contentProvider,
    const LegacyHeightmapTerrainCache& legacyHeightmapTerrainCache) {
    assert(legacyHeightmapTerrainProvider &&
           "forHeightmapTerrainSurfacePath requires a heightmap TerrainProvider");
    return TileContentAccess(
        tileRegistry,
        tileScheme,
        legacyHeightmapTerrainProvider,
        contentProvider,
        &legacyHeightmapTerrainCache,
        TerrainOwnership::HeightmapSurface);
}

TileContentAccess::TileContentAccess(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TerrainProvider* legacyHeightmapTerrainProvider,
    const TilesetContentProvider* contentProvider,
    const LegacyHeightmapTerrainCache* legacyHeightmapTerrainCache,
    TerrainOwnership terrainOwnership)
    : tileRegistry_(tileRegistry),
      tileScheme_(tileScheme),
      contentProvider_(contentProvider),
      terrainOwnership_(terrainOwnership),
      legacyHeightmapContent_(
          legacyHeightmapTerrainProvider,
          contentProvider,
          tileScheme,
          legacyHeightmapTerrainCache) {}

TilesetTile* TileContentAccess::ensureTile(const TileKey& key) {
    TilesetTile* tile = tileRegistry_.ensureTile(
        key,
        tileScheme_,
        contentProvider_);
    if (tile && contentProviderOwnsTerrainQuadtree()) {
        tile->contentProviderTerrainQuadtreeTile = true;
        TileContentTerrainResiduePolicy::clearRejectableResidue(*tile);
    }
    return tile;
}

TileChildFrameMaterializeResult
TileContentAccess::ensureTileChildren(
    TilesetTile& tile,
    IPrepareRendererResources* pPrepRenderer) {
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(tile.key)) {
        const std::vector<TileKey> childKeys =
            contentProviderOwnsTerrainQuadtree() && contentProvider_
                ? contentProvider_->childTiles(tile.key)
                : TileSelectionRootPolicy::levelZeroTerrainRoots(
                      tile.key.schemeId);
        bool changed = false;
        for (const TileKey& childKey : childKeys) {
            TilesetTile* child = ensureTile(childKey);
            if (!child) {
                continue;
            }
            changed |= initializeTerrainChild(
                tile,
                *child,
                contentProviderOwnsTerrainQuadtree(),
                pPrepRenderer);
            changed |= linkChildIfMissing(tile, *child);
        }
        return TileChildFrameMaterializeResult{changed, false};
    }

    TileChildFrameMaterializeResult result =
        TileChildFrameMaterializer::ensureChildren(
        TileChildFrameMaterializeInput{
            tile,
            contentProvider_ && !contentProviderOwnsTerrainQuadtree()
                ? contentProvider_->childTiles(tile.key)
                : std::vector<TileKey>{},
            tileScheme_.maxZoom(),
            hasTerrainQuadtree(),
            isAvailabilityBoundaryTile(tile) &&
                !hasResolvedAvailabilityBoundaryContent(tile),
            contentProviderOwnsTerrainQuadtree(),
            pPrepRenderer},
        [this](const TileKey& key) {
            return ensureTile(key);
        },
        [this](const TileKey& key) {
            return availabilityState(key);
        });
    if (contentProviderOwnsTerrainQuadtree() && contentProvider_) {
        TileTerrainAvailabilityUpsampleBookkeepingPolicy::
            applyMaterializationResult(
            *contentProvider_,
            tile,
            result.terrainUpsampleBookkeeping);
    }
    return result;
}

bool TileContentAccess::hasResolvedAvailabilityBoundaryContent(
    const TilesetTile& tile) const {
    return TileLoadStatePredicates::hasResolvedAvailabilityBoundaryContent(
        tile.content.loadState);
}

bool TileContentAccess::isAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    if (contentProviderOwnsTerrainQuadtree()) {
        return contentTerrainAvailabilityBoundaryTile(tile);
    }
    return legacyHeightmapContent_.isAvailabilityBoundaryTile(tile);
}

bool TileContentAccess::contentProviderOwnsTerrainQuadtree() const {
    return terrainOwnership_ == TerrainOwnership::ContentProvider;
}

bool TileContentAccess::contentTerrainAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    return contentProvider_->isTerrainAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::hasTerrainQuadtree() const {
    return contentProviderOwnsTerrainQuadtree() ||
           terrainOwnership_ == TerrainOwnership::HeightmapSurface;
}

bool TileContentAccess::canRefine(const TilesetTile& tile) const {
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(tile.key)) {
        return true;
    }

    if (contentProviderOwnsTerrainQuadtree()) {
        return TileRefinementAvailabilityResolver::canRefineContentTerrain(
            tile,
            *contentProvider_,
            tileScheme_,
            [this](const TilesetTile& candidate) {
                return isAvailabilityBoundaryTile(candidate);
            },
            [](const TilesetTile& candidate) {
                return TileLoadStatePredicates::
                    hasResolvedAvailabilityBoundaryContent(
                        candidate.content.loadState);
            });
    }

    return legacyHeightmapContent_.canRefine(tile);
}

TileAvailabilityState TileContentAccess::availabilityState(
    const TileKey& key) const {
    if (contentProviderOwnsTerrainQuadtree()) {
        return contentTerrainAvailabilityState(key);
    }
    return legacyHeightmapContent_.availabilityState(key);
}

TileAvailabilityState TileContentAccess::contentTerrainAvailabilityState(
    const TileKey& key) const {
    return contentProvider_->terrainAvailabilityState(key);
}

} // namespace earth_engine
