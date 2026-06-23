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
    const TerrainProvider* legacyTerrainProvider,
    const TilesetContentProvider* contentProvider,
    const TileContentLifecycleManager& contentLifecycle,
    size_t rasterOverlayCount)
    : tileRegistry_(tileRegistry),
      tileScheme_(tileScheme),
      contentProvider_(contentProvider),
      contentLifecycle_(contentLifecycle),
      rasterOverlayCount_(rasterOverlayCount) {
    if (!contentProviderOwnsTerrainQuadtree()) {
        legacyTerrainProvider_ = legacyTerrainProvider;
    }
}

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
            }
            TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                *child,
                tile);
            if (contentProviderOwnsTerrainQuadtree() &&
                !child->content.renderContent.hasGltfContent() &&
                (child->content.renderContent.hasRenderableTerrainContent() ||
                 child->content.renderContent.hasRetainedHeightmap() ||
                 child->content.renderContent.isRenderContentReady() ||
                 child->rasterOverlayState.mappingCount() > 0 ||
                 child->rasterOverlayState.hasMissingProjections())) {
                child->content.renderContent.clearRenderContent();
                child->rasterOverlayState.releaseAndClearReferences(nullptr);
                TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                    *child,
                    tile);
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
    return legacyAvailabilityBoundaryTile(tile);
}

bool TileContentAccess::contentProviderOwnsTerrainQuadtree() const {
    return contentProvider_ && contentProvider_->providesTerrainQuadtree();
}

bool TileContentAccess::legacyAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    return legacyTerrainProvider_ &&
           legacyTerrainProvider_->isAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::contentTerrainAvailabilityBoundaryTile(
    const TilesetTile& tile) const {
    return contentProvider_->isTerrainAvailabilityBoundaryLevel(tile.key.z);
}

bool TileContentAccess::hasTerrainQuadtree() const {
    return contentProviderOwnsTerrainQuadtree() ||
           legacyTerrainProvider_ != nullptr;
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
        legacyTerrainProvider_,
        tileScheme_,
        contentLifecycle_.legacyTerrainCache(),
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
    return legacyAvailabilityState(key);
}

TileAvailabilityState TileContentAccess::legacyAvailabilityState(
    const TileKey& key) const {
    return legacyTerrainProvider_
        ? legacyTerrainProvider_->availabilityState(key)
        : TileAvailabilityState::NotAvailable;
}

TileAvailabilityState TileContentAccess::contentTerrainAvailabilityState(
    const TileKey& key) const {
    return contentProvider_->terrainAvailabilityState(key);
}

} // namespace earth_engine
