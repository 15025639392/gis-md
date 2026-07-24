#include "TerrainProvider.h"
#include <algorithm>
#include <cmath>

namespace earth_engine {

bool DecodedHeightmap::isNoData(float height) const {
    // OpenGlobus RgbTerrain.checkNoDataValue: heights > 50000 are no-data.
    if (height > 50000.0f) return true;

    for (float sentinel : noDataValues) {
        if (height == sentinel) return true;
    }
    return false;
}

float DecodedHeightmap::sampleBilinear(float u, float v) const {
    if (!valid()) return 0.0f;

    // Clamp to [0, 1]
    u = std::max(0.0f, std::min(1.0f, u));
    v = std::max(0.0f, std::min(1.0f, v));

    // 边界内缩:u/v∈[0,1] 映射到 [inset, tileSize-1-inset]。inset=0 时退化为
    // 原顶点栅格映射 [0, tileSize-1](自产 grid65);inset=0.5 时映射到瓦片真实
    // 边界所在的半像素处(Mapbox 514 cell-registered + 重叠环)→ 无缝。
    const float inset = borderInset;
    const float span = static_cast<float>(tileSize - 1) - 2.0f * inset;
    float fx = inset + u * span;
    float fy = inset + v * span;

    int x0 = static_cast<int>(fx);
    int y0 = static_cast<int>(fy);
    int x1 = std::min(x0 + 1, tileSize - 1);
    int y1 = std::min(y0 + 1, tileSize - 1);

    float wx = fx - static_cast<float>(x0);
    float wy = fy - static_cast<float>(y0);

    const float h00 = heights[static_cast<size_t>(y0 * tileSize + x0)];
    const float h10 = heights[static_cast<size_t>(y0 * tileSize + x1)];
    const float h01 = heights[static_cast<size_t>(y1 * tileSize + x0)];
    const float h11 = heights[static_cast<size_t>(y1 * tileSize + x1)];

    // Exclude no-data corners from the blend. A raw bilinear mix of a no-data
    // sentinel (e.g. 65535) with valid corners yields a mid-range value (e.g.
    // ~15000 m) that slips UNDER isNoData's >50000 test → a spurious height
    // ramp/spike along every no-data boundary. Weight only the valid corners
    // and renormalize; if all four are no-data, return a no-data value so the
    // caller's isNoData(result) trips (preserving the "no data here" signal).
    const float w00 = (1.0f - wx) * (1.0f - wy);
    const float w10 = wx * (1.0f - wy);
    const float w01 = (1.0f - wx) * wy;
    const float w11 = wx * wy;

    float weightedSum = 0.0f;
    float weightTotal = 0.0f;
    const auto accumulate = [&](float h, float w) {
        if (!isNoData(h)) {
            weightedSum += h * w;
            weightTotal += w;
        }
    };
    accumulate(h00, w00);
    accumulate(h10, w10);
    accumulate(h01, w01);
    accumulate(h11, w11);

    if (weightTotal <= 0.0f) {
        // All four corners are no-data → propagate a no-data value.
        return h00;
    }
    return weightedSum / weightTotal;
}

} // namespace earth_engine
