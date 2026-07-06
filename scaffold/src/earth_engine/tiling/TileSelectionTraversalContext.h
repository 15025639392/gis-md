#pragma once

#include "TileLoadTypes.h"
#include "TileChildFrameMaterializer.h"
#include "TileIncrementalFrontier.h"
#include "TileSelectionCounters.h"
#include "TileTraversalDetails.h"
#include "../core/math/Vec3.h"

#include <limits>
#include <vector>

namespace earth_engine {

class ActivatedRasterOverlay;
class RenderDevice;
class FrameResourceBudget;
class TileContentAccess;
class Tileset;
struct TilesetTile;
struct TileKey;
class TileLoadQueue;
struct TilePlan;
struct TilesetOptions;
enum class TileOcclusionState;

struct TileSelectionTraversalContext {
    // Two collaborators are genuinely injected — occlusion (software occlusion
    // vs. none) and onVisitTile (live tileset registration vs. shadow-tree
    // active-set reset) — so they stay function pointers. Everything else has a
    // single concrete implementation and is called directly by the executor.
    using CheckOcclusionFn =
        TileOcclusionState (*)(void*, const TilesetTile&);
    using OnVisitTileFn = void (*)(void*, TilesetTile&);

    TilePlan& tilePlan;
    TileLoadQueue& loadQueue;
    TileSelectionCounters& counters;
    const TilesetOptions& options;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    RenderDevice* device = nullptr;
    FrameResourceBudget& frameResourceBudget;
    Vec3 lastCameraPosition = Vec3::zero();
    // Camera cartographic, precomputed once per frame (cartesianToCartographic
    // is an iterative solve and lastCameraPosition is constant across the
    // traversal — recomputing it per visited tile was pure redundant work).
    double cameraLongitude = 0.0;
    double cameraLatitude = 0.0;

    // The one concrete collaborator the executor holds a handle to (the rest
    // are stateless sibling policies called by their static methods). No type
    // erasure — the indirection only ever bound one implementation and blocked
    // inlining of the hot per-visit callbacks.
    TileContentAccess& contentAccess;

    void* occlusionUserData = nullptr;
    CheckOcclusionFn checkOcclusionFn = nullptr;
    OnVisitTileFn onVisitTileFn = nullptr;
    void* onVisitTileUserData = nullptr;

    TileOcclusionState checkOcclusion(const TilesetTile& tile) const {
        return checkOcclusionFn(occlusionUserData, tile);
    }

    // Register + lazily reset a tile at the start of its visit. Injected: the
    // live path registers into the tileset's active-set; the shadow path resets
    // and accumulates into the shadow tree's active-set.
    void onVisitTile(TilesetTile& tile) const {
        if (onVisitTileFn) {
            onVisitTileFn(onVisitTileUserData, tile);
        }
    }

    // ③ 增量切面缓存(incrementalSelection 开启时非空,否则 nullptr → 捕获全是
    // no-op,全量/影子路径零开销)。子树 visit 期间捕获净贡献,见
    // TileIncrementalFrontier。Layer 1 只捕获不剪枝。
    TileIncrementalFrontier* incremental = nullptr;

    // 子树 visit 进入前快照(no-op 空快照当 incremental==nullptr)。
    TileIncrementalFrontier::Snapshot beginIncrementalSubtree() const {
        if (!incremental) return {};
        return incremental->snapshot(tilePlan, loadQueue, counters);
    }

    // 子树 visit 退出时记录净贡献,原样返回 details(便于在 return 处链式调用)。
    TileTraversalDetails recordIncrementalSubtree(
        const TilesetTile& tile,
        const TileIncrementalFrontier::Snapshot& snap,
        const TileTraversalDetails& details,
        double tileMargin) const {
        if (incremental) {
            incremental->record(
                tile, snap, tilePlan, loadQueue, counters, details, tileMargin);
        }
        return details;
    }

    // Per-view distances scratch, reused across every tile visited in this
    // traversal so summarizeForViews doesn't heap-allocate a fresh vector per
    // tile. Mirrors cesium-native's context scratchDistances. Left out of the
    // builder's aggregate initializer on purpose — default-constructs empty and
    // grows to its steady-state capacity on the first frame.
    std::vector<double> scratchDistances;

};

} // namespace earth_engine
