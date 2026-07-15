#pragma once

#include "TileSelectionShadowTree.h"
#include "TileLoadQueue.h"
#include "TilePlan.h"
#include "TileSelectionCounters.h"
#include "../core/math/Vec3.h"
#include "../core/resources/FrameResourceBudget.h"
#include "../scene/FrameState.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class TileScheme;
class TilesetContentProvider;
struct TilesetOptions;
struct TilesetTile;
enum class TileOcclusionState;

/// Per-frame inputs for the worker's shadow selection pass, split off from the
/// live registry so it can be a self-contained VALUE snapshot (for the async
/// worker) that touches no live per-frame state.
///
/// The pointer members (tileScheme/contentProvider/options) reference
/// long-lived Tileset state that is not mutated per frame, so they are safe to
/// hold across the thread boundary. frameState is copied BY VALUE (it carries
/// the per-frame camera-derived selector views the render thread would
/// otherwise reuse for the next frame).
struct TileSelectionShadowSelectInput {
    const TileScheme* tileScheme = nullptr;
    const TilesetContentProvider* contentProvider = nullptr;
    bool contentProviderOwnsTerrainQuadtree = false;
    bool useVirtualTerrainRoot = false;
    const TilesetOptions* options = nullptr;
    FrameState frameState;
    Vec3 lastCameraPosition = Vec3::zero();
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
    // Occlusion evaluated on shadow tiles. If null, every tile is NotOccluded.
    // The async worker MUST pass null here — a real occlusion thunk reads live
    // mutable tileset state (occlusionCameraContext_) that the render thread
    // updates, which would race. Occlusion on the worker is deferred with the
    // glTF/raster read-surface (real-path wiring).
    TileOcclusionState (*checkOcclusion)(void*, const TilesetTile&) = nullptr;
    void* occlusionUserData = nullptr;
};

/// Inputs for a whole synchronous shadow selection pass (build + select). All
/// references must outlive run().
struct TileSelectionShadowRunInput {
    const TilesetTileRegistry& liveRegistry;
    const TileScheme& tileScheme;
    const TilesetContentProvider* contentProvider = nullptr;
    bool contentProviderOwnsTerrainQuadtree = false;
    bool useVirtualTerrainRoot = false;
    const TilesetOptions& options;
    const FrameState& frameState;
    Vec3 lastCameraPosition = Vec3::zero();
    bool interactionActive = false;
    bool resourceSmoothingActive = false;
    TileOcclusionState (*checkOcclusion)(void*, const TilesetTile&) = nullptr;
    void* occlusionUserData = nullptr;
};

/// Runs the ordinary selection traversal (+ finalize) on a shadow copy of the
/// live tile tree, producing a selection result that touches zero live state.
///
/// The heavy 89%-of-selection traversal work runs here against the shadow; the
/// caller reconciles the result back onto live tiles (copy render/load keys,
/// counters, write back selectionState for cross-frame history). Because it is
/// the SAME executor + helpers over a faithful read-surface mirror, the shadow
/// result is byte-identical to the synchronous path (golden-by-construction).
///
/// Scope: content-less selection (empty raster overlays, no glTF render
/// content) — the golden correctness oracle's domain. See TileSelectionShadowTree
/// for the deferred glTF/raster read-surface. Gated behind asyncSelection.
class TileSelectionShadowRunner {
public:
    /// Whole synchronous pass: build the shadow from live, then select on it.
    /// Render-thread only (reads live during the build).
    void run(const TileSelectionShadowRunInput& input);

    /// Phase 1 (render thread): rebuild the shadow tree from the live registry.
    /// Reads live — the caller must ensure the worker is idle (not selecting on
    /// the previous shadow) before calling this.
    void buildShadow(const TilesetTileRegistry& liveRegistry);

    /// Phase 2 (worker thread): run the traversal + finalize on the shadow tree
    /// already built by buildShadow(). Reads only the shadow + the value
    /// snapshot in `input`; touches no live state.
    void selectOnShadow(const TileSelectionShadowSelectInput& input);

    TilePlan& tilePlan() { return shadowTilePlan_; }
    const TilePlan& tilePlan() const { return shadowTilePlan_; }
    const TileLoadQueue& loadQueue() const { return shadowLoadQueue_; }
    const TileSelectionCounters& counters() const { return shadowCounters_; }
    const std::vector<TilesetTile*>& activeTiles() const {
        return shadowActiveTiles_;
    }
    double deltaSeconds() const { return deltaSeconds_; }
    TileSelectionShadowTree& shadowTree() { return shadowTree_; }
    const TileSelectionShadowTree& shadowTree() const { return shadowTree_; }

private:
    TileSelectionShadowTree shadowTree_;
    TilePlan shadowTilePlan_;
    TileLoadQueue shadowLoadQueue_;
    TileSelectionCounters shadowCounters_;
    FrameResourceBudget shadowBudget_;
    std::unordered_set<std::string> shadowFadingOut_;
    std::vector<TilesetTile*> shadowActiveTiles_;
    double deltaSeconds_ = 0.0;
    // Deliberately empty: content-less scope means no required overlays. Kept as
    // a member so the traversal/finalize binding can hold a stable reference.
    std::vector<ActivatedRasterOverlay*> shadowOverlays_;
};

} // namespace earth_engine
