#pragma once

#include "TileLoadPriorityPolicy.h"
#include "TileRefine.h"

#include <cstddef>
#include <vector>

namespace earth_engine {

enum class TileSelectionCullReason {
    None,
    Frustum,
    Fog
};

struct TileSelectionCullResult {
    bool culled = false;
    bool shouldVisit = true;
    TileSelectionCullReason reason = TileSelectionCullReason::None;
};

struct TileSelectionCullLoadPlan {
    bool queueLoad = false;
    TileLoadPriorityGroup group = TileLoadPriorityGroup::Preload;
};

struct TileSelectionCullingPolicy {
    static bool shouldUseChildrenBounds(
        TileRefine refine,
        bool hasChildren,
        bool hasUnconditionallyRefinedChild);

    static bool anyViewVisibleInFog(
        const std::vector<double>& distances,
        const std::vector<double>& fogDensities);

    static TileSelectionCullResult evaluateFrustum(
        bool visibleFromCamera,
        bool enableFrustumCulling);

    static TileSelectionCullResult evaluateFog(
        const TileSelectionCullResult& current,
        bool visibleInFog,
        bool enableFogCulling);

    static TileSelectionCullLoadPlan planCulledTileLoad(
        bool preloadSiblings,
        bool forbidHoles);

    static bool meetsScreenSpaceError(
        bool culled,
        double screenSpaceError,
        double maximumScreenSpaceError,
        bool enforceCulledScreenSpaceError,
        double culledScreenSpaceError);
};

} // namespace earth_engine
