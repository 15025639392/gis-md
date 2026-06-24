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

const HeightmapTerrainCache& emptyHeightmapTerrainCache() {
    static const HeightmapTerrainCache empty;
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
    const TerrainProvider* heightmapTerrainProvider,
    const TilesetContentProvider* contentProvider,
    const HeightmapTerrainCache& heightmapTerrainCache,
    size_t rasterOverlayCount) {
    assert(heightmapTerrainProvider &&
           "forHeightmapTerrainSurfacePath requires a heightmap TerrainProvider");
    return TileContentAccess(
        tileRegistry,
        tileScheme,
        heightmapTerrainProvider,
        contentProvider,
        &heightmapTerrainCache,
        TerrainOwnership::HeightmapSurface,
        rasterOverlayCount);
}

TileContentAccess::TileContentAccess(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TerrainProvider* heightmapTerrainProvider,
    const TilesetContentProvider* contentProvider,
    const HeightmapTerrainCache* heightmapTerrainCache,
    TerrainOwnership terrainOwnership,
    size_t rasterOverlayCount)
    : tileRegistry_(tileRegistry),
      tileScheme_(tileScheme),
      heightmapTerrainProvider_(heightmapTerrainProvider),
      contentProvider_(contentProvider),
      heightmapTerrainCache_(heightmapTerrainCache),
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
    return heightmapAvailabilityBoundaryTile(tile);
}

bool TileContentAccess::contentProviderOwnsTerrainQuadtree() const {
    return terrainOwnership_ == TerrainOwnership::ContentProvider;
}

bool TileContentAccess::heightmapAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    return heightmapTerrainProvider_ &&
           heightmapTerrainProvider_->isAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::contentTerrainAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    return contentProvider_->isTerrainAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::hasTerrainQuadtree() const {
    return contentProviderOwnsTerrainQuadtree() ||
           terrainOwnership_ == TerrainOwnership::HeightmapSurface;
}

bool TileContentAccess::usesHeightmapSurfacePath() const {
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

    return TileRefinementAvailabilityResolver::canRefine(
        tile,
        contentProvider_,
        heightmapTerrainProvider_,
        tileScheme_,
        heightmapTerrainCache_ ? *heightmapTerrainCache_
                               : emptyHeightmapTerrainCache(),
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
    return heightmapAvailabilityState(key);
}

TileAvailabilityState TileContentAccess::heightmapAvailabilityState(
    const TileKey& key) const {
    return heightmapTerrainProvider_
        ? heightmapTerrainProvider_->availabilityState(key)
        : TileAvailabilityState::NotAvailable;
}

TileAvailabilityState TileContentAccess::contentTerrainAvailabilityState(
    const TileKey& key) const {
    return contentProvider_->terrainAvailabilityState(key);
}

} // namespace earth_engine
