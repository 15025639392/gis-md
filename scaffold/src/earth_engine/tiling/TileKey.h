#pragma once

#include "SchemeId.h"

#include <string>
#include <ostream>
#include <functional>

namespace earth_engine {

/// 瓦片键。
/// 标识瓦片体系中的单个瓦片。缓存 key 必须额外包含 provider/layer/style/version。
struct TileKey {
    SchemeId schemeId;  // e.g. "XYZ-WebMercator"（interned handle,拷贝/比较/哈希 O(1)）
    int z = 0;
    int x = 0;
    int y = 0;

    TileKey parent() const;
    int invertedX(int tilesAtLevelX) const;
    int invertedY(int tilesAtLevelY) const;

    bool operator==(const TileKey& rhs) const {
        return schemeId == rhs.schemeId && z == rhs.z && x == rhs.x && y == rhs.y;
    }

    bool operator!=(const TileKey& rhs) const { return !(*this == rhs); }
};

std::ostream& operator<<(std::ostream& os, const TileKey& key);

} // namespace earth_engine

// std::hash 特化（用于 unordered_map）
namespace std {
template<>
struct hash<earth_engine::TileKey> {
    size_t operator()(const earth_engine::TileKey& k) const {
        size_t h = std::hash<earth_engine::SchemeId>()(k.schemeId);
        h ^= std::hash<int>()(k.z) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.x) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>()(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
} // namespace std
