#include "TileSelectionFrameBuilder.h"

#include "../core/geodesy/Ellipsoid.h"

namespace earth_engine {

SelectorFrame TileSelectionFrameBuilder::build(
    const FrameState& frameState,
    const std::vector<FogDensityAtHeight>& fogDensityTable) {
    SelectorFrame selectorFrame;
    selectorFrame.views = frameState.selectorViews;
    selectorFrame.fogDensities.reserve(selectorFrame.views.size());
    for (const auto& view : selectorFrame.views) {
        const double viewHeight =
            Ellipsoid::WGS84().cartesianToCartographic(view.position).height();
        selectorFrame.fogDensities.push_back(
            TileSelectionMetrics::computeFogDensity(
                fogDensityTable,
                viewHeight));
    }
    return selectorFrame;
}

} // namespace earth_engine
