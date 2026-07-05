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
struct TilesetTile;
struct TileKey;
class TileLoadQueue;
struct TilePlan;
struct TilesetOptions;
enum class TileOcclusionState;

struct TileSelectionTraversalContext {
    using QueueTileLoadFn = void (*)(
        void*,
        const TileKey&,
        TileLoadPriorityGroup,
        double);
    using AddTileToCurrentPlanFn = void (*)(
        void*,
        TilesetTile&,
        double,
        bool,
        double);
    using EnsureTileChildrenFn =
        TileChildFrameMaterializeResult (*)(void*, TilesetTile&);
    using CanRefineFn = bool (*)(void*, const TilesetTile&);
    using CheckOcclusionFn =
        TileOcclusionState (*)(void*, const TilesetTile&);
    using HasLodTransitionRenderContentFn =
        bool (*)(void*, const TilesetTile&);
    using CreateTraversalDetailsFn =
        TileTraversalDetails (*)(void*, const TilesetTile&);
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

    void* userData = nullptr;
    void* contentAccessUserData = nullptr;
    QueueTileLoadFn queueTileLoadFn = nullptr;
    AddTileToCurrentPlanFn addTileToCurrentPlanFn = nullptr;
    EnsureTileChildrenFn ensureTileChildrenFn = nullptr;
    CanRefineFn canRefineFn = nullptr;
    CheckOcclusionFn checkOcclusionFn = nullptr;
    HasLodTransitionRenderContentFn hasLodTransitionRenderContentFn = nullptr;
    CreateTraversalDetailsFn createSingleTileDetailsFn = nullptr;
    CreateTraversalDetailsFn createCulledTileDetailsFn = nullptr;
    OnVisitTileFn onVisitTileFn = nullptr;
    void* onVisitTileUserData = nullptr;

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

    // Register a tile into this frame's selection active-set (and lazily reset
    // it once). Called at the start of every tile visit so the per-frame reset
    // touches only visited tiles instead of the whole registry.
    void onVisitTile(TilesetTile& tile) const {
        if (onVisitTileFn) {
            onVisitTileFn(onVisitTileUserData, tile);
        }
    }

    void queueTileLoad(const TileKey& key,
                       TileLoadPriorityGroup group,
                       double priority) const {
        queueTileLoadFn(userData, key, group, priority);
    }

    void addTileToCurrentPlan(TilesetTile& tile,
                              double screenSpaceError,
                              bool queueForLoad,
                              double priority) const {
        addTileToCurrentPlanFn(
            userData,
            tile,
            screenSpaceError,
            queueForLoad,
            priority);
    }

    TileChildFrameMaterializeResult ensureTileChildren(
        TilesetTile& tile) const {
        return ensureTileChildrenFn(contentAccessUserData, tile);
    }

    bool canRefine(const TilesetTile& tile) const {
        return canRefineFn(contentAccessUserData, tile);
    }

    TileOcclusionState checkOcclusion(const TilesetTile& tile) const {
        return checkOcclusionFn(userData, tile);
    }

    bool hasLodTransitionRenderContent(const TilesetTile& tile) const {
        return hasLodTransitionRenderContentFn(userData, tile);
    }

    TileTraversalDetails createSingleTileDetails(
        const TilesetTile& tile) const {
        return createSingleTileDetailsFn(userData, tile);
    }

    TileTraversalDetails createCulledTileDetails(
        const TilesetTile& tile) const {
        return createCulledTileDetailsFn(userData, tile);
    }
};

} // namespace earth_engine
