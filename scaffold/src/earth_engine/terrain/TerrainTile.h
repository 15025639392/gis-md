#pragma once

#include "../providers/TerrainProvider.h"
#include "../tiling/TileKey.h"
#include "../core/math/Rectangle.h"
#include <memory>

namespace earth_engine {

class TileScheme;

/// 地形瓦片。
/// 持有解码后的高度图数据，提供地理坐标→高度的双线性采样。
class TerrainTile {
public:
    /// @param key 瓦片键
    /// @param scheme 瓦片体系（用于 tile→bounds 转换）
    /// @param heightmap 解码后的高度图（所有权转移）
    TerrainTile(TileKey key,
                const TileScheme& scheme,
                std::unique_ptr<DecodedHeightmap> heightmap);

    const TileKey& key() const { return key_; }
    const Rectangle& bounds() const { return bounds_; }
    const DecodedHeightmap* heightmap() const { return heightmap_.get(); }
    bool valid() const { return heightmap_ && heightmap_->valid(); }

    /// 双线性采样高度。
    /// @param lngRad 经度（radian）
    /// @param latRad 纬度（radian）
    /// @param parentTile 可选的父瓦片（用于 no-data 回退）
    /// @return ellipsoid height（meter），超出范围返回 0
    float sampleHeight(double lngRad, double latRad,
                       const TerrainTile* parentTile = nullptr) const;

private:
    TileKey key_;
    Rectangle bounds_;
    std::unique_ptr<DecodedHeightmap> heightmap_;
};

} // namespace earth_engine
