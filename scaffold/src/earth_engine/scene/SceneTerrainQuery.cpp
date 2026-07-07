#include "SceneTerrainQuery.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../tiling/Tileset.h"

namespace earth_engine {

std::function<float(double, double)>
SceneTerrainQuery::makeLngLatHeightSampler(const Tileset* terrainTileset) {
    if (!terrainTileset) {
        return {};
    }
    return [terrainTileset](double lng, double lat) {
        return terrainTileset->sampleHeight(lng, lat);
    };
}

std::optional<double> SceneTerrainQuery::sampleHeight(
    const Tileset* terrainTileset,
    const Vec3& ecefPosition) {
    if (!terrainTileset) {
        return std::nullopt;
    }
    const Cartographic c =
        Ellipsoid::WGS84().cartesianToCartographic(ecefPosition);
    const std::optional<float> h =
        terrainTileset->sampleHeightOptional(c.longitude(), c.latitude());
    if (!h) {
        return std::nullopt;
    }
    return static_cast<double>(*h);
}

} // namespace earth_engine
