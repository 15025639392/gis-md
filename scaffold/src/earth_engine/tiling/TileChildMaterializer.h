#pragma once

#include "TileKey.h"
#include "TileBoundsMetrics.h"
#include "TileBoundingVolume.h"
#include "TileTerrainHeightRangePolicy.h"
#include "TilesetTile.h"
#include "../core/math/Rectangle.h"
#include "../providers/TerrainProvider.h"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace earth_engine {

struct TileRefinementAvailabilityOptions {
    bool hasExistingChildren = false;
    bool hasContentChildren = false;
    bool contentProviderSupportsTile = false;
    bool isAvailabilityBoundaryWaitingForContent = false;
    bool hasTerrainQuadtree = false;
    bool cachedHeightmapCanRefine = true;
    int maxZoom = 0;
};

struct TileChildMaterializer {
    template <typename EnsureTileFn>
    static bool linkContentChildren(
        TilesetTile& parent,
        const std::vector<TileKey>& childKeys,
        EnsureTileFn&& ensureTile) {
        bool changed = false;
        for (const TileKey& childKey : childKeys) {
            TilesetTile* child = ensureTile(childKey);
            if (!child) continue;
            changed |= linkChild(parent, *child);
        }
        return changed;
    }

    template <typename AvailabilityFn, typename EnsureTileFn>
    static bool materializeTerrainChildren(
        TilesetTile& parent,
        int maxZoom,
        AvailabilityFn&& availabilityState,
        EnsureTileFn&& ensureTile) {
        if (parent.key.z >= maxZoom ||
            parent.content.isTerrainAvailabilityUpsample()) {
            return false;
        }

        const int childZ = parent.key.z + 1;
        const int childX = parent.key.x * 2;
        const int childY = parent.key.y * 2;

        struct ChildAvailability {
            TileKey key;
            TileAvailabilityState state = TileAvailabilityState::NotAvailable;
        };
        std::vector<ChildAvailability> children;
        children.reserve(4);
        bool anyChildAvailable = false;
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                TileKey childKey{
                    parent.key.schemeId,
                    childZ,
                    childX + dx,
                    childY + dy};
                if (parent.key.schemeId == "Geographic-TMS") {
                    if (childKey.x < 0 ||
                        childKey.x >= geographicTmsXCount(childZ) ||
                        childKey.y < 0 ||
                        childKey.y >= geographicTmsYCount(childZ)) {
                        continue;
                    }
                }
                const TileAvailabilityState state =
                    availabilityState(childKey);
                anyChildAvailable |=
                    state == TileAvailabilityState::Available;
                children.push_back(ChildAvailability{childKey, state});
            }
        }

        // cesium-native LayerJsonTerrainLoader::createTileChildrenImpl:
        // if any child is available, create all children. Non-available
        // children are UpsampledQuadtreeNode equivalents and must not request
        // terrain data.
        if (!anyChildAvailable) {
            return false;
        }

        bool changed = false;
        for (const ChildAvailability& childInfo : children) {
            TilesetTile* child = ensureTile(childInfo.key);
            if (!child) continue;
            child->geometricError = parent.geometricError * 0.5;
            child->refine = parent.refine;
            const double minimumHeight =
                TileBoundsMetrics::terrainMinimumHeight(parent);
            const double maximumHeight =
                TileBoundsMetrics::terrainMaximumHeight(parent);
            child->boundingVolume = TileBoundingVolume::fromRegion(
                child->bounds,
                minimumHeight,
                maximumHeight);
            child->contentBoundingVolume = child->boundingVolume;
            if (!child->content.renderContent.isRenderContentReady()) {
                TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                    *child,
                    parent);
            }

            const bool upsampled =
                childInfo.state != TileAvailabilityState::Available;
            if (child->content.isTerrainAvailabilityUpsample() != upsampled ||
                child->content.isRasterDetailUpsample()) {
                child->content.renderContent.clearRenderContent();
                if (upsampled) {
                    child->content.markTerrainAvailabilityUpsample();
                } else {
                    child->content.clearUpsampleKind();
                }
                changed = true;
            }
            changed |= linkChild(parent, *child);
        }
        return changed;
    }

    template <typename EnsureTileFn>
    static bool materializeRasterUpsampledChildren(
        TilesetTile& parent,
        const Rectangle& subdivisionRectangle,
        double defaultGeometricError,
        EnsureTileFn&& ensureTile) {
        return materializeRasterUpsampledChildren(
            parent,
            subdivisionRectangle,
            subdivisionRectangle.center(),
            defaultGeometricError,
            std::forward<EnsureTileFn>(ensureTile));
    }

    template <typename EnsureTileFn>
    static bool materializeRasterUpsampledChildren(
        TilesetTile& parent,
        const Rectangle& subdivisionRectangle,
        const std::pair<double, double>& subdivisionCenter,
        double defaultGeometricError,
        EnsureTileFn&& ensureTile) {
        if (parent.children.size() >= 4) {
            return false;
        }

        const double centerLng = subdivisionCenter.first;
        const double centerLat = subdivisionCenter.second;

        const int childZ = parent.key.z + 1;
        const int childX = parent.key.x * 2;
        const int childY = parent.key.y * 2;
        const std::array<Rectangle, 4> childBounds = {
            Rectangle(
                subdivisionRectangle.west(),
                subdivisionRectangle.south(),
                centerLng,
                centerLat),
            Rectangle(
                centerLng,
                subdivisionRectangle.south(),
                subdivisionRectangle.east(),
                centerLat),
            Rectangle(
                subdivisionRectangle.west(),
                centerLat,
                centerLng,
                subdivisionRectangle.north()),
            Rectangle(
                centerLng,
                centerLat,
                subdivisionRectangle.east(),
                subdivisionRectangle.north())
        };

        parent.refine = TileRefine::Replace;
        if (parent.geometricError <= 0.0) {
            parent.geometricError = defaultGeometricError;
        }

        bool changed = false;
        for (size_t i = 0; i < childBounds.size(); ++i) {
            const int dx = static_cast<int>(i % 2);
            const int dy = static_cast<int>(i / 2);
            TileKey childKey{
                parent.key.schemeId,
                childZ,
                childX + dx,
                childY + dy};
            TilesetTile* child = ensureTile(childKey);
            if (!child) {
                continue;
            }

            child->parent = &parent;
            child->bounds = childBounds[i];
            child->boundingVolume = TileBoundingVolume::fromRegion(
                childBounds[i],
                TileBoundsMetrics::terrainMinimumHeight(parent),
                TileBoundsMetrics::terrainMaximumHeight(parent));
            child->contentBoundingVolume = child->boundingVolume;
            child->geometricError = parent.geometricError * 0.5;
            child->refine = TileRefine::Replace;
            child->content.markRasterDetailUpsample();
            child->unconditionallyRefine = false;
            TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                *child,
                parent);

            changed |= linkChild(parent, *child);
        }
        return changed;
    }

    template <typename CacheKeyFn,
              typename IsTerrainCachedFn,
              typename AvailabilityStateFn>
    static bool canRefine(
        const TilesetTile& tile,
        const TileRefinementAvailabilityOptions& options,
        CacheKeyFn&& cacheKey,
        IsTerrainCachedFn&& isTerrainCached,
        AvailabilityStateFn&& availabilityState) {
        if (tile.content.isTerrainAvailabilityUpsample()) {
            return false;
        }

        if (options.isAvailabilityBoundaryWaitingForContent) {
            return false;
        }

        if (options.hasExistingChildren) {
            return true;
        }

        if (options.hasContentChildren) {
            return true;
        }
        if (options.contentProviderSupportsTile) {
            return false;
        }

        if (tile.key.z >= options.maxZoom) {
            return false;
        }

        const int childZ = tile.key.z + 1;
        const int childX = tile.key.x * 2;
        const int childY = tile.key.y * 2;
        for (int dy = 0; dy < 2; ++dy) {
            for (int dx = 0; dx < 2; ++dx) {
                TileKey childKey{
                    tile.key.schemeId,
                    childZ,
                    childX + dx,
                    childY + dy};
                if (tile.key.schemeId == "Geographic-TMS") {
                    if (childKey.x < 0 ||
                        childKey.x >= geographicTmsXCount(childZ) ||
                        childKey.y < 0 ||
                        childKey.y >= geographicTmsYCount(childZ)) {
                        continue;
                    }
                }
                if (options.cachedHeightmapCanRefine &&
                    isTerrainCached(cacheKey(childKey))) {
                    return true;
                }
                if (options.hasTerrainQuadtree &&
                    availabilityState(childKey) ==
                        TileAvailabilityState::Available) {
                    return true;
                }
            }
        }
        return false;
    }

private:
    static int geographicTmsXCount(int z) {
        return 1 << (z + 1);
    }

    static int geographicTmsYCount(int z) {
        return 1 << z;
    }

    static bool linkChild(TilesetTile& parent, TilesetTile& child) {
        child.parent = &parent;
        if (std::find(parent.children.begin(),
                      parent.children.end(),
                      &child) != parent.children.end()) {
            return false;
        }
        parent.children.push_back(&child);
        return true;
    }
};

} // namespace earth_engine
