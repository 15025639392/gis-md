#pragma once

#include "../core/math/Vec3.h"

#include <functional>

namespace earth_engine {

class Tileset;

class SceneTerrainQuery {
public:
    static std::function<float(double, double)> makeLngLatHeightSampler(
        const Tileset* terrainTileset);
    static double sampleHeight(const Tileset* terrainTileset,
                               const Vec3& ecefPosition);
};

} // namespace earth_engine
