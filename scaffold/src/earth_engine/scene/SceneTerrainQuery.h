#pragma once

#include "../core/math/Vec3.h"

#include <functional>
#include <optional>

namespace earth_engine {

class Tileset;

class SceneTerrainQuery {
public:
    static std::function<float(double, double)> makeLngLatHeightSampler(
        const Tileset* terrainTileset);
    // nullopt = 无地形数据(未加载 / 无覆盖瓦片 / 无地形 tileset),供相机 clamp
    // 区分真实海平面 0 与"未知",避免在未加载高地形处下沉。
    static std::optional<double> sampleHeight(const Tileset* terrainTileset,
                                              const Vec3& ecefPosition);
};

} // namespace earth_engine
