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
/// NOT yet mirrored (intentionally): the glTF/raster render-content
/// read-surface (hasGltfContent / isRenderContentReady / raster mapping
/// readiness / terrain height range). Those only affect the on-device path's
/// renderable/refine decisions; the golden oracle uses content-less tiles and
/// empty raster overlays, so they are added later when the real async path is
/// wired and device-validated. Until then async selection is gated off.
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
