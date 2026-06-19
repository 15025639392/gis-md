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

double SceneTerrainQuery::sampleHeight(
    const Tileset* terrainTileset,
    const Vec3& ecefPosition) {
    if (!terrainTileset) {
        return 0.0;
    }
    const Cartographic c =
        Ellipsoid::WGS84().cartesianToCartographic(ecefPosition);
    return static_cast<double>(
        terrainTileset->sampleHeight(c.longitude(), c.latitude()));
}

} // namespace earth_engine
