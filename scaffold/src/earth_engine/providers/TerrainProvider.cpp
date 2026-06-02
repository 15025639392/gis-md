#include "TerrainProvider.h"
#include <algorithm>
#include <cmath>

namespace earth_engine {

float DecodedHeightmap::sampleBilinear(float u, float v) const {
    if (!valid()) return 0.0f;

    // Clamp to [0, 1]
    u = std::max(0.0f, std::min(1.0f, u));
    v = std::max(0.0f, std::min(1.0f, v));

    float fx = u * static_cast<float>(tileSize - 1);
    float fy = v * static_cast<float>(tileSize - 1);

    int x0 = static_cast<int>(fx);
    int y0 = static_cast<int>(fy);
    int x1 = std::min(x0 + 1, tileSize - 1);
    int y1 = std::min(y0 + 1, tileSize - 1);

    float wx = fx - static_cast<float>(x0);
    float wy = fy - static_cast<float>(y0);

    float h00 = heights[static_cast<size_t>(y0 * tileSize + x0)];
    float h10 = heights[static_cast<size_t>(y0 * tileSize + x1)];
    float h01 = heights[static_cast<size_t>(y1 * tileSize + x0)];
    float h11 = heights[static_cast<size_t>(y1 * tileSize + x1)];

    float top = h00 + wx * (h10 - h00);
    float bottom = h01 + wx * (h11 - h01);
    return top + wy * (bottom - top);
}

} // namespace earth_engine
