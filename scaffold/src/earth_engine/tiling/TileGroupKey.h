#pragma once

#include <string>
#include <ostream>
#include <functional>

namespace earth_engine {

/// 瓦片分组键。
/// 同 scheme + 同 zoom + 同 viewport 的图层共享一次 TilePlan 计算。
/// 用于 BasemapLayerStack 去重 TilePlan 计算。
struct TileGroupKey {
    std::string schemeId;
    int zoom = 0;
    int viewportWidthPixels = 0;
    int viewportHeightPixels = 0;

    bool operator==(const TileGroupKey& rhs) const {
        return schemeId == rhs.schemeId
            && zoom == rhs.zoom
            && viewportWidthPixels == rhs.viewportWidthPixels
            && viewportHeightPixels == rhs.viewportHeightPixels;
    }

    bool operator!=(const TileGroupKey& rhs) const { return !(*this == rhs); }
};

std::ostream& operator<<(std::ostream& os, const TileGroupKey& key);

} // namespace earth_engine

// std::hash 特化（用于 unordered_map）
namespace std {
template<>
struct hash<earth_engine::TileGroupKey> {
    size_t operator()(const earth_engine::TileGroupKey& k) const {
        size_t h = std::hash<std::string>()(k.schemeId);
        h ^= std::hash<int>()(k.zoom) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.viewportWidthPixels) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.viewportHeightPixels) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
} // namespace std
