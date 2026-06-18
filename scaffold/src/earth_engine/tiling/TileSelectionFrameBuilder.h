#pragma once

#include "TileSelectionMetrics.h"

#include "../scene/FrameState.h"

#include <vector>

namespace earth_engine {

struct SelectorFrame {
    std::vector<FrameState::SelectorView> views;
    std::vector<double> fogDensities;
};

struct TileSelectionFrameBuilder {
    static SelectorFrame build(
        const FrameState& frameState,
        const std::vector<FogDensityAtHeight>& fogDensityTable);
};

} // namespace earth_engine
