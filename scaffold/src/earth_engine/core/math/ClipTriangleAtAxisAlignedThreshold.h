#pragma once

#include <cmath>
#include <limits>
#include <variant>
#include <vector>

namespace earth_engine {

struct InterpolatedVertex {
    int first = 0;
    int second = 0;
    double t = 0.0;

    constexpr bool operator==(const InterpolatedVertex& other) const noexcept {
        return first == other.first &&
               second == other.second &&
               std::fabs(t - other.t) <= std::numeric_limits<double>::epsilon();
    }

    constexpr bool operator!=(const InterpolatedVertex& other) const noexcept {
        return !(*this == other);
    }
};

using TriangleClipVertex = std::variant<int, InterpolatedVertex>;

void clipTriangleAtAxisAlignedThreshold(
    double threshold,
    bool keepAbove,
    int i0,
    int i1,
    int i2,
    double u0,
    double u1,
    double u2,
    std::vector<TriangleClipVertex>& result) noexcept;

} // namespace earth_engine
