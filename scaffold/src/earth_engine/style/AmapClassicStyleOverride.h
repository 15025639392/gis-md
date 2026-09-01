#pragma once

#include "earth_engine/data/StyleExpression.h"
#include "earth_engine/layers/FeatureRenderLayer.h"

#include <array>
#include <vector>

namespace earth_engine {

/// Runtime override layer on top of the sealed official AMap classic style.
/// Overrides are applied AFTER the official profile is installed, so they only
/// replace specific identity colors/widths and never reorder or re-identify
/// official geometry. Constant-value only: an override pins a styleGroup
/// (classCode*1000+subKey) to a fixed color/width for the layer that owns it.
/// Style is sealed; callers cannot introduce a second class-ordering table.
struct AmapClassicStyleOverrides {
    struct Surface {
        int classCode = 0;
        int subKey = 0;
        std::array<float, 4> color{};
    };
    struct Line {
        int classCode = 0;
        int subKey = 0;
        std::array<float, 4> color{};
        float widthPx = 0.0f;
    };
    std::vector<Surface> surface;
    std::vector<Line> line;
    bool empty() const { return surface.empty() && line.empty(); }
};

void applyAmapClassicStyleOverrides(
    FeatureRenderStyle& style, const AmapClassicStyleOverrides& overrides);

} // namespace earth_engine