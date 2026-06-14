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

float TerrainTile::sampleHeight(double lngRad, double latRad,
                                 const TerrainTile* parentTile) const {
    if (!valid()) {
        if (parentTile) return parentTile->sampleHeight(lngRad, latRad);
        return 0.0f;
    }

    // 计算归一化坐标 [0,1]
    double u = (lngRad - bounds_.west()) / bounds_.width();
    double v = (bounds_.north() - latRad) / bounds_.height();  // 北→南

    // QM-rasterized grids cover the full tile without padding.
    // No skirt-trim remapping is needed (unlike Mapbox RGB terrain).

    // 超出范围 → 尝试父瓦片
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) {
        if (parentTile) return parentTile->sampleHeight(lngRad, latRad);
        return 0.0f;
    }

    float h = heightmap_->sampleBilinear(
        static_cast<float>(u), static_cast<float>(v));

    // OpenGlobus no-data → parent fallback
    if ((h == 0.0f || heightmap_->isNoData(h)) && parentTile) {
        float parentH = parentTile->sampleHeight(lngRad, latRad);
        // OpenGlobus skipPositiveHeights at low zoom: if parent zoom ≤ 8,
        // positive parent heights are treated as sea level (0)
        if (parentTile->key().z <= 8 && parentH > 0.0f) {
            return 0.0f;
        }
        return parentH;
    }

    return h;
}

} // namespace earth_engine
