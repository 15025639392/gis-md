#include "TerrainProvider.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace earth_engine {

bool DecodedHeightmap::isNoData(float height) const {
    // OpenGlobus RgbTerrain.checkNoDataValue: heights > 50000 are no-data.
    if (height > 50000.0f) return true;

    for (float sentinel : noDataValues) {
        if (height == sentinel) return true;
    }
    return false;
}

void DecodedHeightmap::assignHeights(const std::vector<float>& rawHeights) {
    stagedHeights = rawHeights;
    assignHeights();
    releaseStagedHeights();
}

void DecodedHeightmap::releaseStagedHeights() {
    stagedHeights.clear();
    stagedHeights.shrink_to_fit();
}

void DecodedHeightmap::assignHeights() {
    const std::vector<float>& rawHeights = stagedHeights;
    quantizedHeights.clear();
    quantizedHeights.reserve(rawHeights.size());

    // min/max **只统计有效高度**:nodata(Terrain-RGB 为 -10000m)混进来会把
    // 量化原点拉到 -10000,量程白白吃掉一大截。
    float minH = 1e30f;
    float maxH = -1e30f;
    for (float h : rawHeights) {
        if (isNoData(h)) continue;
        if (h < minH) minH = h;
        if (h > maxH) maxH = h;
    }
    const bool hasValid = minH <= maxH;
    minHeight = hasValid ? minH : 0.0f;
    maxHeight = hasValid ? maxH : 0.0f;

    // 全局固定格点:格点下标 n = round(h / kQuantStep) 与瓦片无关,故同一物理
    // 高度在相邻瓦片解出**逐位相同**的 float(无缝不变量,见头文件注释)。
    // quantBase 只是把 n 平移进 [1, 65535] 的存储窗口,不影响 n 本身。
    const int64_t minNode = hasValid
        ? static_cast<int64_t>(std::floor(minHeight / kQuantStep))
        : 0;
    quantBase = static_cast<int32_t>(minNode - 1);

    for (float h : rawHeights) {
        if (isNoData(h)) {
            quantizedHeights.push_back(0);  // 保留码
            continue;
        }
        const int64_t node =
            static_cast<int64_t>(std::llround(h / kQuantStep));
        int64_t code = node - static_cast<int64_t>(quantBase);
        // 瓦内起伏超过 8191.75m 才会触顶(现实瓦片不会);钳住而不是回绕。
        code = std::max<int64_t>(1, std::min<int64_t>(kMaxQuantCode, code));
        quantizedHeights.push_back(static_cast<uint16_t>(code));
    }
}


float DecodedHeightmap::overscanReach() const {
    if (!valid() || borderInset <= 0.0f) return 0.0f;
    const float span = static_cast<float>(tileSize - 1) - 2.0f * borderInset;
    if (span <= 0.0f) return 0.0f;
    return borderInset / span;
}

float DecodedHeightmap::sampleBilinear(float u, float v) const {
    // Clamp to [0, 1]
    return sampleBilinearUnclamped(std::max(0.0f, std::min(1.0f, u)),
                                   std::max(0.0f, std::min(1.0f, v)));
}

float DecodedHeightmap::sampleBilinearUnclamped(float u, float v) const {
    if (!valid()) return 0.0f;

    // 边界内缩:u/v∈[0,1] 映射到 [inset, tileSize-1-inset]。inset=0 时退化为
    // 原顶点栅格映射 [0, tileSize-1](自产 grid65);inset=0.5 时映射到瓦片真实
    // 边界所在的半像素处(Mapbox 514 cell-registered + 重叠环)→ 无缝。
    const float inset = borderInset;
    const float span = static_cast<float>(tileSize - 1) - 2.0f * inset;
    float fx = inset + u * span;
    float fy = inset + v * span;

    // 只钳像素下标,不钳 u/v —— u/v 越界 ±overscanReach() 时落在重叠环上,
    // 读到的是**真实邻瓦数据**(法线边界差分靠这个,见 overscanReach 注释)。
    // floor 而非截断:fx 可为负(inset=0 的源越界时),截断会朝零取整。
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    const float wx = fx - static_cast<float>(x0);
    const float wy = fy - static_cast<float>(y0);
    int x1 = std::min(x0 + 1, tileSize - 1);
    int y1 = std::min(y0 + 1, tileSize - 1);
    x0 = std::max(0, std::min(x0, tileSize - 1));
    y0 = std::max(0, std::min(y0, tileSize - 1));
    x1 = std::max(0, x1);
    y1 = std::max(0, y1);

    const float h00 = heightAt(static_cast<size_t>(y0 * tileSize + x0));
    const float h10 = heightAt(static_cast<size_t>(y0 * tileSize + x1));
    const float h01 = heightAt(static_cast<size_t>(y1 * tileSize + x0));
    const float h11 = heightAt(static_cast<size_t>(y1 * tileSize + x1));

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
