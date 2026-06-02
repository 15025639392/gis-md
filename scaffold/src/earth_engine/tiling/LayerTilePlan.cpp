#include "LayerTilePlan.h"
#include "TileScheme.h"
#include "../scene/Camera.h"

namespace earth_engine {

// ============================================================
// TileGroupKey ostream
// ============================================================

std::ostream& operator<<(std::ostream& os, const TileGroupKey& key) {
    return os << "TileGroupKey(" << key.schemeId
              << " z=" << key.zoom
              << " vp=" << key.viewportWidthPixels << "x" << key.viewportHeightPixels
              << ")";
}

// ============================================================
// LayerTilePlan
// ============================================================

LayerTilePlan LayerTilePlan::independent(TilePlan plan) {
    LayerTilePlan result;
    result.owned_ = std::make_unique<TilePlan>(std::move(plan));
    result.plan_ = result.owned_.get();
    return result;
}

LayerTilePlan LayerTilePlan::sharedFrom(const TilePlan* shared) {
    LayerTilePlan result;
    result.plan_ = shared;
    return result;
}

// ============================================================
// TilePlanGroupBuilder
// ============================================================

std::unordered_map<TileGroupKey, TilePlan>
TilePlanGroupBuilder::computeGrouped(
    const std::vector<const TileScheme*>& schemes,
    const Camera& camera,
    double viewportWidthPixels,
    double viewportHeightPixels) {

    std::unordered_map<TileGroupKey, TilePlan> grouped;

    for (const auto* scheme : schemes) {
        if (!scheme) continue;

        TilePlan plan = TilePlanBuilder::compute(
            camera, *scheme, viewportWidthPixels, viewportHeightPixels);

        TileGroupKey key{scheme->id(), plan.zoom,
                         static_cast<int>(viewportWidthPixels),
                         static_cast<int>(viewportHeightPixels)};

        // 每组只计算一次（第一个图层触发计算）
        if (grouped.find(key) == grouped.end()) {
            grouped[key] = std::move(plan);
        }
    }

    return grouped;
}

} // namespace earth_engine
