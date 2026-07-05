#pragma once

#include "TileSelectionShadowTree.h"
#include "TileLoadQueue.h"
#include "TilePlan.h"
#include "TileSelectionCounters.h"
#include "../core/math/Vec3.h"
#include "../core/resources/FrameResourceBudget.h"

#include <string>
#include <unordered_set>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class TileScheme;
class TilesetContentProvider;
struct TilesetOptions;
struct FrameState;
struct TilesetTile;
enum class TileOcclusionState;

/// Inputs for one shadow selection pass. All references must outlive run().
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
    // Occlusion evaluated on shadow tiles (bounds + camera based, so it works on
    // a shadow tile just as on the live tile). If null, every tile is treated
    // as NotOccluded.
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
    void run(const TileSelectionShadowRunInput& input);

    TilePlan& tilePlan() { return shadowTilePlan_; }
    const TilePlan& tilePlan() const { return shadowTilePlan_; }
    const TileLoadQueue& loadQueue() const { return shadowLoadQueue_; }
    const TileSelectionCounters& counters() const { return shadowCounters_; }
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
    // Deliberately empty: content-less scope means no required overlays. Kept as
    // a member so the traversal/finalize binding can hold a stable reference.
    std::vector<ActivatedRasterOverlay*> shadowOverlays_;
};

} // namespace earth_engine
