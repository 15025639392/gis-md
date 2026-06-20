#include "TileSelectionCullingPolicy.h"

#include "TileSelectionMetrics.h"

#include <algorithm>

namespace earth_engine {

bool TileSelectionCullingPolicy::shouldUseChildrenBounds(
    TileRefine refine,
    bool hasChildren,
    bool hasUnconditionallyRefinedChild) {
    return refine == TileRefine::Replace &&
           hasChildren &&
           !hasUnconditionallyRefinedChild;
}

bool TileSelectionCullingPolicy::anyViewVisibleInFog(
    const std::vector<double>& distances,
    const std::vector<double>& fogDensities) {
    const size_t count = std::min(distances.size(), fogDensities.size());
    for (size_t i = 0; i < count; ++i) {
        if (TileSelectionMetrics::isVisibleInFog(
                distances[i],
                fogDensities[i])) {
            return true;
        }
    }
    return false;
}

TileSelectionCullResult TileSelectionCullingPolicy::evaluateFrustum(
    bool visibleFromCamera,
    bool enableFrustumCulling) {
    TileSelectionCullResult result;
    if (!visibleFromCamera) {
        result.culled = true;
        result.reason = TileSelectionCullReason::Frustum;
        if (enableFrustumCulling) {
            result.shouldVisit = false;
        }
    }
    return result;
}

TileSelectionCullResult TileSelectionCullingPolicy::evaluateFog(
    const TileSelectionCullResult& current,
    bool visibleInFog,
    bool enableFogCulling) {
    if (!current.shouldVisit || current.culled || visibleInFog) {
        return current;
    }

    TileSelectionCullResult result = current;
    result.culled = true;
    result.reason = TileSelectionCullReason::Fog;
    if (enableFogCulling) {
        result.shouldVisit = false;
    }
    return result;
}

TileSelectionCullLoadPlan TileSelectionCullingPolicy::planCulledTileLoad(
    bool preloadSiblings,
    bool forbidHoles,
    TileRefine refine) {
    if (forbidHoles && refine == TileRefine::Replace) {
        return TileSelectionCullLoadPlan{
            true,
            TileLoadPriorityGroup::Normal};
    }

    if (preloadSiblings) {
        return TileSelectionCullLoadPlan{
            true,
            TileLoadPriorityGroup::Preload};
    }

    return TileSelectionCullLoadPlan{};
}

bool TileSelectionCullingPolicy::meetsScreenSpaceError(
    bool culled,
    double screenSpaceError,
    double maximumScreenSpaceError,
    bool enforceCulledScreenSpaceError,
    double culledScreenSpaceError) {
    if (culled) {
        return !enforceCulledScreenSpaceError ||
               screenSpaceError < culledScreenSpaceError;
    }
    return screenSpaceError < maximumScreenSpaceError;
}

} // namespace earth_engine
