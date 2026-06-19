#pragma once

#include "TileLoadTypes.h"
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
    using EnsureTileChildrenFn = void (*)(void*, TilesetTile&);
    using CanRefineFn = bool (*)(void*, const TilesetTile&);
    using CheckOcclusionFn =
        TileOcclusionState (*)(void*, const TilesetTile&);
    using HasLodTransitionRenderContentFn =
        bool (*)(void*, const TilesetTile&);
    using CreateTraversalDetailsFn =
        TileTraversalDetails (*)(void*, const TilesetTile&);

    TilePlan& tilePlan;
    TileLoadQueue& loadQueue;
    TileSelectionCounters& counters;
    const TilesetOptions& options;
    const std::vector<ActivatedRasterOverlay*>& rasterOverlays;
    RenderDevice* device = nullptr;
    FrameResourceBudget& frameResourceBudget;
    Vec3 lastCameraPosition = Vec3::zero();

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

    void ensureTileChildren(TilesetTile& tile) const {
        ensureTileChildrenFn(contentAccessUserData, tile);
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
