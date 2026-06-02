#include "TerrainTile.h"
#include "../tiling/TileScheme.h"
#include <algorithm>
#include <cmath>

namespace earth_engine {

TerrainTile::TerrainTile(TileKey key,
                          const TileScheme& scheme,
                          std::unique_ptr<DecodedHeightmap> heightmap)
    : key_(std::move(key)),
      bounds_(scheme.tileToRectangle(key_)),
      heightmap_(std::move(heightmap)) {}

float TerrainTile::sampleHeight(double lngRad, double latRad) const {
    if (!valid()) return 0.0f;

    // 计算归一化坐标 [0,1]
    double u = (lngRad - bounds_.west()) / bounds_.width();
    double v = (bounds_.north() - latRad) / bounds_.height();  // 北→南

    // 超出范围返回 0
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) return 0.0f;

    return heightmap_->sampleBilinear(
        static_cast<float>(u), static_cast<float>(v));
}

} // namespace earth_engine
