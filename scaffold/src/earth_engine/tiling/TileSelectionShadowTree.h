#pragma once

#include "TilesetTileRegistry.h"

#include <cstddef>

namespace earth_engine {

struct TilesetTile;
struct TileKey;

/// A parallel ("shadow") copy of the live tile tree's SELECTION read-surface.
///
/// Built on the render thread from the live registry; the selection worker runs
/// the ordinary traversal on this shadow so the selection algorithm stays
/// byte-identical to the synchronous path (golden-by-construction) while
/// touching zero live pointers / GPU resources. See the async-selection plan
/// (asyncSelection kill-switch, default false).
///
/// Scope (step 1 — snapshot builder): mirrors the content-less selection
/// read-surface the golden correctness oracle exercises:
///   - geometry / structure: bounds, geometricError, refine,
///     unconditionallyRefine, boundingVolume / viewerRequestVolume /
///     contentBoundingVolume (+ initial variants),
///   - load / content classification: content.loadState, content.contentKind,
///     content.contentUpsampleKind (+ rasterDetailSourceProjection),
///   - cross-frame selection history: selectionFrameState.previousSelectionState
///     and selectionState,
///   - tree topology: parent / children (mirrored by key, order preserved).
///
/// glTF render-content readiness IS mirrored (plain booleans only — no model
/// or GPU resources): hasGltfContent / isRenderContentReady /
/// isTerrainRenderContent + terrain height range, via
/// TileRenderContentState::setShadowReadinessMirror. This makes the shadow's
/// renderable / kick / refine / upsample-descent decisions match the live tile
/// on device. Golden tiles are content-less, so every mirror value is false and
/// the oracle is unaffected.
///
/// NOT yet mirrored (deferred, real-path follow-up): the RASTER overlay mapping
/// read-surface (per-mapping hasPendingNonPlaceholderLoadingTile /
/// isMoreDetailAvailable / hasReadyMapping). The shadow runs with empty
/// overlays, so it does not gate refinement on imagery readiness; on device this
/// only makes the shadow refine slightly more eagerly than sync until imagery
/// catches up (render entries still resolve against live content). Occlusion is
/// likewise deferred on the async worker (NotOccluded).
class TileSelectionShadowTree {
public:
    /// Rebuild the shadow to mirror `liveRegistry`'s current read-surface.
    /// Clears any prior shadow content. Render-thread only.
    void build(const TilesetTileRegistry& liveRegistry);

    TilesetTileRegistry& registry() { return shadowRegistry_; }
    const TilesetTileRegistry& registry() const { return shadowRegistry_; }

    TilesetTile* findShadow(const TileKey& key) {
        return shadowRegistry_.findTile(key);
    }
    const TilesetTile* findShadow(const TileKey& key) const {
        return shadowRegistry_.findTile(key);
    }

    std::size_t size() const { return shadowRegistry_.tiles().size(); }

    /// Copy the selection read-surface (not topology) of one live tile onto a
    /// freshly-created shadow tile. Exposed for reuse by the shadow context
    /// (step 2) when a newly virtual-descended shadow child must be seeded.
    static void mirrorReadSurface(const TilesetTile& live, TilesetTile& shadow);

private:
    TilesetTileRegistry shadowRegistry_;
};

} // namespace earth_engine
