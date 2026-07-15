#include "TileVisibleRangeFinalizer.h"

#include "TilePlan.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace earth_engine {

void TileVisibleRangeFinalizer::dedupeVisibleTiles(TilePlan& plan) {
    const bool handlesAligned =
        plan.tilesToRenderThisFrame.size() == plan.visibleTiles.size();
    std::unordered_set<TileKey> seen;
    std::vector<TileKey> deduped;
    std::vector<TilesetTile*> dedupedHandles;
    deduped.reserve(plan.visibleTiles.size());
    if (handlesAligned) {
        dedupedHandles.reserve(plan.tilesToRenderThisFrame.size());
    }
    for (size_t i = 0; i < plan.visibleTiles.size(); ++i) {
        const TileKey& key = plan.visibleTiles[i];
        if (seen.insert(key).second) {
            deduped.push_back(key);
            if (handlesAligned) {
                dedupedHandles.push_back(
                    plan.tilesToRenderThisFrame[i]);
            }
        }
    }
    plan.visibleTiles = std::move(deduped);
    if (handlesAligned) {
        plan.tilesToRenderThisFrame = std::move(dedupedHandles);
    } else {
        plan.tilesToRenderThisFrame.clear();
    }
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
