#include "TileVisibleRangeFinalizer.h"

#include "TilePlan.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace earth_engine {

void TileVisibleRangeFinalizer::dedupeVisibleTiles(TilePlan& plan) {
    std::unordered_set<TileKey> seen;
    std::vector<TileKey> deduped;
    deduped.reserve(plan.visibleTiles.size());
    for (const TileKey& key : plan.visibleTiles) {
        if (seen.insert(key).second) {
            deduped.push_back(key);
        }
    }
    plan.visibleTiles = std::move(deduped);
}

void TileVisibleRangeFinalizer::updateVisibleZoomRange(TilePlan& plan) {
    if (plan.visibleTiles.empty()) {
        return;
    }

    plan.minVisibleZoom = plan.visibleTiles.front().z;
    plan.maxVisibleZoom = plan.visibleTiles.front().z;
    for (const TileKey& key : plan.visibleTiles) {
        plan.minVisibleZoom = std::min(plan.minVisibleZoom, key.z);
        plan.maxVisibleZoom = std::max(plan.maxVisibleZoom, key.z);
    }
    plan.zoom = plan.maxVisibleZoom;
}

} // namespace earth_engine
