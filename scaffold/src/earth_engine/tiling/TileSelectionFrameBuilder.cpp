#include "TileSelectionFrameBuilder.h"

#include "../core/geodesy/Ellipsoid.h"

#include <algorithm>

namespace earth_engine {

SelectorFrame TileSelectionFrameBuilder::build(
    const FrameState& frameState,
    const std::vector<FogDensityAtHeight>& fogDensityTable) {
    SelectorFrame selectorFrame;
    selectorFrame.views = frameState.selectorViews;
    selectorFrame.fogDensities.reserve(selectorFrame.views.size());
    for (const auto& view : selectorFrame.views) {
        const double viewHeight = std::max(
            0.0,
            Ellipsoid::WGS84().cartesianToCartographic(view.position).height());
        selectorFrame.fogDensities.push_back(
            TileSelectionMetrics::computeFogDensity(
                fogDensityTable,
                viewHeight));
    }
    return selectorFrame;
}

} // namespace earth_engine
