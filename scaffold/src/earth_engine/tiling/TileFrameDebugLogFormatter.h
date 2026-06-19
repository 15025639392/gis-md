#pragma once

#include "TileRenderDebugLogInput.h"
#include "TileUpdateDebugLogInput.h"

#include <array>

namespace earth_engine {

class TileFrameDebugLogFormatter {
public:
    static std::array<char, 512> updateDetail(
        const TileUpdateDebugLogInput& input);

    static std::array<char, 512> renderBuildDetail(
        const TileRenderDebugLogInput& input);
};

} // namespace earth_engine
