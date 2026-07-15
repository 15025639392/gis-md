#include "TileContentAccess.h"

#include "TileCacheKey.h"
#include "TileChildFrameMaterializer.h"
#include "TileBoundsMetrics.h"
#include "TileContentTerrainResiduePolicy.h"
#include "TileContentLifecycleManager.h"
#include "TileContentResourceInvalidator.h"
#include "TileLoadStatePredicates.h"
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
    return parent.attachChild(child);
}

bool initializeTerrainChild(
    TilesetTile& parent,
    TilesetTile& child,
    bool clearResidue,
    IPrepareRendererResources* pPrepRenderer) {
    bool changed = false;
    if (!child.boundingVolume) {
        child.setBoundingVolume(TileBoundingVolume::fromLooseRegion(
            child.bounds,
            TileBoundsMetrics::terrainMinimumHeight(parent),
            TileBoundsMetrics::terrainMaximumHeight(parent)));
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
    const TilesetContentProvider& contentProvider,
    TileContentResourceInvalidator* resourceInvalidator) {
    return TileContentAccess(
        tileRegistry,
        tileScheme,
        &contentProvider,
        true,
        resourceInvalidator);
}

TileContentAccess TileContentAccess::forNoTerrain(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TilesetContentProvider* contentProvider,
    TileContentResourceInvalidator* resourceInvalidator) {
    return TileContentAccess(
        tileRegistry,
        tileScheme,
        contentProvider,
        false,
        resourceInvalidator);
}

TileContentAccess::TileContentAccess(
    TilesetTileRegistry& tileRegistry,
    const TileScheme& tileScheme,
    const TilesetContentProvider* contentProvider,
    bool contentProviderOwnsTerrainQuadtree,
    TileContentResourceInvalidator* resourceInvalidator)
    : tileRegistry_(tileRegistry),
      tileScheme_(tileScheme),
      contentProvider_(contentProvider),
      contentProviderOwnsTerrainQuadtree_(contentProviderOwnsTerrainQuadtree),
      resourceInvalidator_(resourceInvalidator) {}

TilesetTile* TileContentAccess::ensureTile(
    const TileKey& key,
    IPrepareRendererResources* pPrepRenderer) {
    TilesetTile* tile = tileRegistry_.ensureTile(
        key,
        tileScheme_,
        contentProvider_);
    if (tile && contentProviderOwnsTerrainQuadtree()) {
        if (TileContentTerrainResiduePolicy::clearRejectableResidue(
                *tile,
                pPrepRenderer) &&
            resourceInvalidator_) {
            resourceInvalidator_->markTileResourcesChanged(*tile);
        }
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
            TilesetTile* child = ensureTile(childKey, pPrepRenderer);
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
        if (changed && resourceInvalidator_) {
            for (TilesetTile* child : tile.children) {
                if (child) {
                    resourceInvalidator_->markTileResourcesChanged(*child);
                }
            }
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
            pPrepRenderer,
            contentProvider_ ? contentProvider_->childTopologyRevision() : 0},
        [this](const TileKey& key) {
            return ensureTile(key);
        },
        [this](const TileKey& key) {
            return availabilityState(key);
        });
    if (result.changed && resourceInvalidator_) {
        for (TilesetTile* child : tile.children) {
            if (child) {
                resourceInvalidator_->markTileResourcesChanged(*child);
            }
        }
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
    return contentProvider_ &&
           contentProvider_->isTerrainAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::contentProviderOwnsTerrainQuadtree() const {
    return contentProviderOwnsTerrainQuadtree_;
}

bool TileContentAccess::contentTerrainAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    return contentProvider_->isTerrainAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::hasTerrainQuadtree() const {
    return contentProviderOwnsTerrainQuadtree();
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

    if (contentProvider_) {
        const std::vector<TileKey> contentChildren =
            contentProvider_->childTiles(tile.key);
        return TileChildMaterializer::canRefine(
            tile,
            TileRefinementAvailabilityOptions{
                !tile.children.empty(),
                !contentChildren.empty(),
                contentProvider_->supportsTile(tile.key),
                isAvailabilityBoundaryTile(tile) &&
                    !hasResolvedAvailabilityBoundaryContent(tile),
                false,
                tileScheme_.maxZoom()},
            [](const TileKey&) {
                return TileAvailabilityState::NotAvailable;
            });
    }

    return false;
}

TileAvailabilityState TileContentAccess::availabilityState(
    const TileKey& key) const {
    if (contentProviderOwnsTerrainQuadtree()) {
        return contentTerrainAvailabilityState(key);
    }
    return TileAvailabilityState::NotAvailable;
}

TileAvailabilityState TileContentAccess::contentTerrainAvailabilityState(
    const TileKey& key) const {
    return contentProvider_->availabilityState(key);
}

} // namespace earth_engine
