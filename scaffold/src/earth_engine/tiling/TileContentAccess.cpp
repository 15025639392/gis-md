#include "TileContentAccess.h"

#include "TileCacheKey.h"
#include "TileChildFrameMaterializer.h"
#include "TileBoundsMetrics.h"
#include "TileContentTerrainResiduePolicy.h"
#include "TileContentLifecycleManager.h"
#include "TileRefinementAvailabilityResolver.h"
#include "TileSelectionRootPolicy.h"
#include "TileScheme.h"
#include "TileTerrainHeightRangePolicy.h"
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

const LegacyHeightmapTerrainCache& emptyLegacyHeightmapTerrainCache() {
    static const LegacyHeightmapTerrainCache empty;
    return empty;
}

} // namespace

TileContentAccess TileContentAccess::forContentTerrain(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TilesetContentProvider& contentProvider,
    size_t rasterOverlayCount) {
    return TileContentAccess(
        tileRegistry,
        tileScheme,
        nullptr,
        &contentProvider,
        nullptr,
        TerrainOwnership::ContentProvider,
        rasterOverlayCount);
}

TileContentAccess TileContentAccess::forNoTerrain(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TilesetContentProvider* contentProvider,
    size_t rasterOverlayCount) {
    return TileContentAccess(
        tileRegistry,
        tileScheme,
        nullptr,
        contentProvider,
        nullptr,
        TerrainOwnership::None,
        rasterOverlayCount);
}

TileContentAccess TileContentAccess::forHeightmapTerrainSurfacePath(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TerrainProvider* legacyHeightmapTerrainProvider,
    const TilesetContentProvider* contentProvider,
    const LegacyHeightmapTerrainCache& legacyHeightmapTerrainCache,
    size_t rasterOverlayCount) {
    assert(legacyHeightmapTerrainProvider &&
           "forHeightmapTerrainSurfacePath requires a heightmap TerrainProvider");
    return TileContentAccess(
        tileRegistry,
        tileScheme,
        legacyHeightmapTerrainProvider,
        contentProvider,
        &legacyHeightmapTerrainCache,
        TerrainOwnership::HeightmapSurface,
        rasterOverlayCount);
}

TileContentAccess::TileContentAccess(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TerrainProvider* legacyHeightmapTerrainProvider,
    const TilesetContentProvider* contentProvider,
    const LegacyHeightmapTerrainCache* legacyHeightmapTerrainCache,
    TerrainOwnership terrainOwnership,
    size_t rasterOverlayCount)
    : tileRegistry_(tileRegistry),
      tileScheme_(tileScheme),
      legacyHeightmapTerrainProvider_(legacyHeightmapTerrainProvider),
      contentProvider_(contentProvider),
      legacyHeightmapTerrainCache_(legacyHeightmapTerrainCache),
      terrainOwnership_(terrainOwnership),
      rasterOverlayCount_(rasterOverlayCount) {}

TilesetTile* TileContentAccess::ensureTile(const TileKey& key) {
    TilesetTile* tile = tileRegistry_.ensureTile(
        key,
        tileScheme_,
        contentProvider_,
        rasterOverlayCount_);
    if (tile && contentProviderOwnsTerrainQuadtree()) {
        tile->contentProviderTerrainQuadtreeTile = true;
        TileContentTerrainResiduePolicy::clearRejectableResidue(*tile);
    }
    return tile;
}

void TileContentAccess::ensureTileChildren(TilesetTile& tile) {
    if (TileSelectionRootPolicy::isVirtualTerrainRoot(tile.key)) {
        const std::vector<TileKey> childKeys =
            contentProviderOwnsTerrainQuadtree() && contentProvider_
                ? contentProvider_->childTiles(tile.key)
                : TileSelectionRootPolicy::levelZeroTerrainRoots(
                      tile.key.schemeId);
        for (const TileKey& childKey : childKeys) {
            TilesetTile* child = ensureTile(childKey);
            if (!child) {
                continue;
            }
            if (!child->boundingVolume) {
                child->boundingVolume = TileBoundingVolume::fromRegion(
                    child->bounds,
                    TileBoundsMetrics::terrainMinimumHeight(tile),
                    TileBoundsMetrics::terrainMaximumHeight(tile));
            }
            TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                *child,
                tile);
            if (contentProviderOwnsTerrainQuadtree()) {
                if (TileContentTerrainResiduePolicy::clearRejectableResidue(
                        *child)) {
                    TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                        *child,
                        tile);
                }
            }
            linkChildIfMissing(tile, *child);
        }
        return;
    }

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
            contentProviderOwnsTerrainQuadtree()},
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
    if (contentProviderOwnsTerrainQuadtree()) {
        return contentTerrainAvailabilityBoundaryTile(tile);
    }
    return legacyHeightmapAvailabilityBoundaryTile(tile);
}

bool TileContentAccess::contentProviderOwnsTerrainQuadtree() const {
    return terrainOwnership_ == TerrainOwnership::ContentProvider;
}

bool TileContentAccess::legacyHeightmapAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    return legacyHeightmapTerrainProvider_ &&
           legacyHeightmapTerrainProvider_->isAvailabilityBoundaryLevel(
               tile.key.z);
}

bool TileContentAccess::contentTerrainAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    return contentProvider_->isTerrainAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::hasTerrainQuadtree() const {
    return contentProviderOwnsTerrainQuadtree() ||
           terrainOwnership_ == TerrainOwnership::HeightmapSurface;
}

bool TileContentAccess::retainsLegacyHeightmapTerrainCacheForLegacySurfacePath()
    const {
    return terrainOwnership_ == TerrainOwnership::HeightmapSurface;
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
                return candidate.content.loadState >
                    TileLoadState::ContentLoading;
            });
    }

    return TileRefinementAvailabilityResolver::
        canRefineLegacyHeightmapSurfaceOrExternalContent(
        tile,
        contentProvider_,
        legacyHeightmapTerrainProvider_,
        tileScheme_,
        legacyHeightmapTerrainCache_ ? *legacyHeightmapTerrainCache_
                                     : emptyLegacyHeightmapTerrainCache(),
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
    if (contentProviderOwnsTerrainQuadtree()) {
        return contentTerrainAvailabilityState(key);
    }
    return legacyHeightmapAvailabilityState(key);
}

TileAvailabilityState TileContentAccess::legacyHeightmapAvailabilityState(
    const TileKey& key) const {
    return legacyHeightmapTerrainProvider_
        ? legacyHeightmapTerrainProvider_->availabilityState(key)
        : TileAvailabilityState::NotAvailable;
}

TileAvailabilityState TileContentAccess::contentTerrainAvailabilityState(
    const TileKey& key) const {
    return contentProvider_->terrainAvailabilityState(key);
}

} // namespace earth_engine
