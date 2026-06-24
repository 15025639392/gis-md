#pragma once

#include "TileKey.h"
#include "TileBoundsMetrics.h"
#include "TileBoundingVolume.h"
#include "TileContentTerrainResiduePolicy.h"
#include "TileQuadtreeChildKeys.h"
#include "TileTerrainHeightRangePolicy.h"
#include "TilesetTile.h"
#include "../core/math/Rectangle.h"
#include "../providers/TerrainProvider.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace earth_engine {

class IPrepareRendererResources;

struct TileRefinementAvailabilityOptions {
    bool hasExistingChildren = false;
    bool hasContentChildren = false;
    bool contentProviderSupportsTile = false;
    bool isAvailabilityBoundaryWaitingForContent = false;
    bool hasTerrainQuadtree = false;
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
            const size_t childCountBefore = parent.children.size();
            TilesetTile* child = ensureTile(childKey);
            if (!child) continue;
            const bool linkedByEnsure =
                parent.children.size() != childCountBefore &&
                std::find(parent.children.begin(), parent.children.end(),
                          child) != parent.children.end();
            changed |= linkedByEnsure || linkChild(parent, *child);
        }
        return changed;
    }

    template <typename AvailabilityFn, typename EnsureTileFn>
    static bool materializeTerrainChildren(
        TilesetTile& parent,
        int maxZoom,
        AvailabilityFn&& availabilityState,
        EnsureTileFn&& ensureTile,
        bool clearTerrainAvailabilityUpsampleContent = false,
        IPrepareRendererResources* pPrepRenderer = nullptr) {
        if (parent.key.z >= maxZoom ||
            parent.content.isTerrainAvailabilityUpsample()) {
            return false;
        }

        struct ChildAvailability {
            TileKey key;
            TileAvailabilityState state = TileAvailabilityState::NotAvailable;
        };
        std::vector<ChildAvailability> children;
        children.reserve(4);
        bool anyChildAvailable = false;
        for (const TileKey& childKey :
             TileQuadtreeChildKeys::terrainChildren(parent.key)) {
            const TileAvailabilityState state = availabilityState(childKey);
            anyChildAvailable |= state == TileAvailabilityState::Available;
            children.push_back(ChildAvailability{childKey, state});
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
            child->unconditionallyRefine = false;
            const bool hasAcceptedTerrainContent =
                TileContentTerrainResiduePolicy::hasAcceptedTerrainContent(
                    *child);
            const double childGeometricError = parent.geometricError * 0.5;
            const double minimumHeight =
                TileBoundsMetrics::terrainMinimumHeight(parent);
            const double maximumHeight =
                TileBoundsMetrics::terrainMaximumHeight(parent);
            const TileBoundingVolume childBoundingVolume =
                TileBoundingVolume::fromLooseRegion(
                    child->bounds,
                    minimumHeight,
                    maximumHeight);
            const bool childTraversalGeometryChanged =
                child->geometricError != childGeometricError ||
                child->refine != parent.refine;
            const bool childBoundsChanged =
                !sameRegionBoundingVolume(
                    child->boundingVolume,
                    childBoundingVolume) ||
                child->contentBoundingVolume.has_value();
            child->geometricError = childGeometricError;
            child->refine = parent.refine;
            if (!hasAcceptedTerrainContent) {
                child->boundingVolume = childBoundingVolume;
                child->contentBoundingVolume.reset();
            }
            const bool childGeometryChanged =
                childTraversalGeometryChanged ||
                (!hasAcceptedTerrainContent && childBoundsChanged);
            changed |= childGeometryChanged;
            if (!hasAcceptedTerrainContent) {
                TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                    *child,
                    parent);
            }

            const bool upsampled =
                childInfo.state != TileAvailabilityState::Available &&
                !hasAcceptedTerrainContent;
            const bool hasStaleTerrainResidue =
                clearTerrainAvailabilityUpsampleContent &&
                !hasAcceptedTerrainContent &&
                TileContentTerrainResiduePolicy::hasRejectableResidue(*child);
            if (child->content.isTerrainAvailabilityUpsample() != upsampled ||
                child->content.isRasterDetailUpsample() ||
                hasStaleTerrainResidue) {
                if (!hasAcceptedTerrainContent) {
                    TileContentTerrainResiduePolicy::clearRejectableResidue(
                        *child,
                        pPrepRenderer);
                }
                if (upsampled) {
                    child->content.markTerrainAvailabilityUpsample();
                } else {
                    child->content.clearUpsampleKind();
                }
                if (!hasAcceptedTerrainContent) {
                    TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                        *child,
                        parent);
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
        EnsureTileFn&& ensureTile,
        IPrepareRendererResources* pPrepRenderer = nullptr) {
        return materializeRasterUpsampledChildren(
            parent,
            subdivisionRectangle,
            subdivisionRectangle.center(),
            defaultGeometricError,
            std::forward<EnsureTileFn>(ensureTile),
            pPrepRenderer);
    }

    template <typename EnsureTileFn>
    static bool materializeRasterUpsampledChildren(
        TilesetTile& parent,
        const Rectangle& subdivisionRectangle,
        const std::pair<double, double>& subdivisionCenter,
        double defaultGeometricError,
        EnsureTileFn&& ensureTile,
        IPrepareRendererResources* pPrepRenderer = nullptr,
        std::optional<RasterOverlayProjection> sourceProjection =
            std::nullopt) {
        if (parent.children.size() >= 4) {
            const bool canRefreshRasterUpsampledChildren =
                parent.children.size() == 4 &&
                std::all_of(
                    parent.children.begin(),
                    parent.children.end(),
                    [](const TilesetTile* child) {
                        return child &&
                               child->content.isRasterDetailUpsample();
                    });
            if (!canRefreshRasterUpsampledChildren) {
                return false;
            }
        }

        const double centerLng = subdivisionCenter.first;
        const double centerLat = subdivisionCenter.second;

        const int64_t childZ64 = static_cast<int64_t>(parent.key.z) + 1;
        const int64_t childX64 = static_cast<int64_t>(parent.key.x) * 2;
        const int64_t childY64 = static_cast<int64_t>(parent.key.y) * 2;
        if (!canRepresentTileCoordinate(childZ64) ||
            !canRepresentTileCoordinate(childX64 + 1) ||
            !canRepresentTileCoordinate(childY64 + 1)) {
            return false;
        }
        const int childZ = static_cast<int>(childZ64);
        const int childX = static_cast<int>(childX64);
        const int childY = static_cast<int>(childY64);
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

            const double childGeometricError = parent.geometricError * 0.5;
            const bool wasRasterDetailUpsample =
                child->content.isRasterDetailUpsample();
            const bool childGeometryChanged =
                child->bounds != childBounds[i] ||
                child->geometricError != childGeometricError;

            child->parent = &parent;
            child->bounds = childBounds[i];
            child->boundingVolume = TileBoundingVolume::fromRegion(
                childBounds[i],
                TileBoundsMetrics::terrainMinimumHeight(parent),
                TileBoundsMetrics::terrainMaximumHeight(parent));
            child->contentBoundingVolume = child->boundingVolume;
            child->geometricError = childGeometricError;
            child->refine = TileRefine::Replace;
            if (!wasRasterDetailUpsample || childGeometryChanged) {
                child->content.renderContent.clearRenderContent();
                child->rasterOverlayState.releaseAndClearReferences(
                    pPrepRenderer);
            }
            if (sourceProjection) {
                child->content.markRasterDetailUpsample(*sourceProjection);
            } else {
                child->content.markRasterDetailUpsample();
            }
            child->unconditionallyRefine = false;
            TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                *child,
                parent);

            changed |= childGeometryChanged;
            changed |= linkChild(parent, *child);
        }
        return changed;
    }

    template <typename AvailabilityStateFn>
    static bool canRefine(
        const TilesetTile& tile,
        const TileRefinementAvailabilityOptions& options,
        AvailabilityStateFn&& availabilityState) {
        if (tile.content.isTerrainAvailabilityUpsample()) {
            return false;
        }

        if (options.hasExistingChildren) {
            return true;
        }

        if (options.isAvailabilityBoundaryWaitingForContent) {
            return false;
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

        for (const TileKey& childKey :
             TileQuadtreeChildKeys::terrainChildren(tile.key)) {
            if (options.hasTerrainQuadtree &&
                availabilityState(childKey) ==
                    TileAvailabilityState::Available) {
                return true;
            }
        }
        return false;
    }

private:
    static bool canRepresentTileCoordinate(int64_t value) {
        return value >= static_cast<int64_t>(std::numeric_limits<int>::min()) &&
               value <= static_cast<int64_t>(std::numeric_limits<int>::max());
    }

    static bool sameRegionBoundingVolume(
        const std::optional<TileBoundingVolume>& existing,
        const TileBoundingVolume& expected) {
        return existing &&
               existing->kind == TileBoundingVolumeKind::Region &&
               expected.kind == TileBoundingVolumeKind::Region &&
               existing->region == expected.region &&
               existing->minimumHeight == expected.minimumHeight &&
               existing->maximumHeight == expected.maximumHeight &&
               existing->looseFittingHeights ==
                   expected.looseFittingHeights;
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
