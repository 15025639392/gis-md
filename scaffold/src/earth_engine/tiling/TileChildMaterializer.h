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
        EnsureTileFn&& ensureTile,
        bool* materializationComplete = nullptr) {
        bool changed = false;
        bool complete = true;
        for (const TileKey& childKey : childKeys) {
            const size_t childCountBefore = parent.children.size();
            TilesetTile* child = ensureTile(childKey);
            if (!child) {
                complete = false;
                continue;
            }
            const bool linkedByEnsure =
                parent.children.size() != childCountBefore &&
                std::find(parent.children.begin(), parent.children.end(),
                          child) != parent.children.end();
            changed |= linkedByEnsure || linkChild(parent, *child);
        }
        if (materializationComplete) {
            *materializationComplete = complete;
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
        IPrepareRendererResources* pPrepRenderer = nullptr,
        bool* materializationComplete = nullptr) {
        if (parent.key.z >= maxZoom ||
            parent.content.isTerrainAvailabilityUpsample()) {
            if (materializationComplete) {
                *materializationComplete = true;
            }
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
            if (materializationComplete) {
                *materializationComplete = true;
            }
            return false;
        }

        bool changed = false;
        bool complete = true;
        for (const ChildAvailability& childInfo : children) {
            TilesetTile* child = ensureTile(childInfo.key);
            if (!child) {
                complete = false;
                continue;
            }
            const bool childUnconditionalRefineChanged =
                child->unconditionallyRefine;
            child->setUnconditionallyRefine(false);
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
            child->setGeometricError(childGeometricError);
            child->setRefine(parent.refine);
            if (!hasAcceptedTerrainContent) {
                if (childBoundsChanged) {
                    child->setBoundingVolume(childBoundingVolume);
                    child->resetContentBoundingVolume();
                }
            }
            const bool childGeometryChanged =
                childTraversalGeometryChanged ||
                (!hasAcceptedTerrainContent && childBoundsChanged);
            changed |=
                childGeometryChanged || childUnconditionalRefineChanged;
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
                    child->markTerrainAvailabilityUpsample();
                } else {
                    child->clearUpsampleKind();
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
        if (materializationComplete) {
            *materializationComplete = complete;
        }
        return changed;
    }

    template <typename EnsureTileFn>
    static bool materializeRasterUpsampledChildren(
        TilesetTile& parent,
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

        parent.setRefine(TileRefine::Replace);
        if (parent.geometricError <= 0.0) {
            parent.setGeometricError(defaultGeometricError);
        }

        bool changed = false;
        for (size_t i = 0; i < 4; ++i) {
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
                child->geometricError != childGeometricError;

            // The child keeps its registry/scheme rectangle (ensureTile
            // computed it from the tile key). These are REAL quadtree tiles
            // that may later receive real content, and selection culls
            // ancestors through the union of children bounding volumes —
            // overwriting bounds with content-derived subdivision quadrants
            // (which drift per upsample level via the imagery-rect union and
            // content-center split) poisons that union and frustum-culls
            // visible tiles. cesium-native avoids this by giving upsampled
            // children separate UpsampledQuadtreeNode IDs that never alias a
            // real tile; this engine reuses the real tile, so the real
            // rectangle must stay authoritative. The upsampled MESH itself is
            // still split at the parent's overlay-texcoord 0.5 independently
            // of these bounds (GltfTerrainUpsampler).
            child->setBoundingVolume(TileBoundingVolume::fromRegion(
                child->bounds,
                TileBoundsMetrics::terrainMinimumHeight(parent),
                TileBoundsMetrics::terrainMaximumHeight(parent)));
            child->setContentBoundingVolume(child->boundingVolume);
            child->setGeometricError(childGeometricError);
            child->setRefine(TileRefine::Replace);
            if (!wasRasterDetailUpsample || childGeometryChanged) {
                child->content.renderContent.clearRenderContent();
                child->rasterOverlayState.releaseAndClearReferences(
                    pPrepRenderer);
                child->notifyChildMaterializationStateChanged();
            }
            if (sourceProjection) {
                child->markRasterDetailUpsample(*sourceProjection);
            } else {
                child->markRasterDetailUpsample();
            }
            child->setUnconditionallyRefine(false);
            TileTerrainHeightRangePolicy::inheritTerrainHeightRange(
                *child,
                parent);

            changed |= childGeometryChanged;
            changed |= linkChild(parent, *child);
        }
        return changed;
    }

    template <typename AvailabilityStateFn>
    static bool hasTerrainAvailabilityUpsampledChild(
        const TilesetTile& tile,
        AvailabilityStateFn&& availabilityState) {
        if (!tile.children.empty()) {
            return std::any_of(
                tile.children.begin(),
                tile.children.end(),
                [](const TilesetTile* child) {
                    return child &&
                           child->content.isTerrainAvailabilityUpsample();
                });
        }

        int availableChildren = 0;
        int totalChildren = 0;
        for (const TileKey& childKey :
             TileQuadtreeChildKeys::terrainChildren(tile.key)) {
            ++totalChildren;
            if (availabilityState(childKey) ==
                TileAvailabilityState::Available) {
                ++availableChildren;
            }
        }
        return totalChildren == 4 && availableChildren > 0 &&
               availableChildren < 4;
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
        return parent.attachChild(child);
    }
};

} // namespace earth_engine
