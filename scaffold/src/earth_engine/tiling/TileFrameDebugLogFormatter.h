#pragma once

#include "TileRenderDebugLogInput.h"
#include "TileUpdateDebugLogInput.h"

#include <array>

namespace earth_engine {

class TileFrameDebugLogFormatter {
public:
    static std::array<char, 384> updateDetail(
        const TileUpdateDebugLogInput& input);

    static std::array<char, 384> renderBuildDetail(
        const TileRenderDebugLogInput& input);
};

} // namespace earth_engine
