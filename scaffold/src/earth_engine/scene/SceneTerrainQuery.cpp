#include "SceneTerrainQuery.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../tiling/Tileset.h"

#include <algorithm>
#include <cmath>

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

void SceneTerrainQuery::sampleAreaHeights(
    const Tileset* terrainTileset,
    const Vec3& groundEcef,
    double radiusMeters,
    const std::vector<glm::dvec2>& localOffsetsMeters,
    std::vector<CameraController::TerrainSample>& out) {
    out.assign(localOffsetsMeters.size(), {});
    if (!terrainTileset || localOffsetsMeters.empty()) {
        return;
    }
    const auto& ellipsoid = Ellipsoid::WGS84();
    const Cartographic center =
        ellipsoid.cartesianToCartographic(groundEcef);
    const double lat0 = center.latitude();
    const double lon0 = center.longitude();
    // 米 → 弧度换算(球近似足够:探针半径 ≤ 20km,纬向误差 <0.4%);经度按
    // cos(lat) 放大,极区钳到下限防除零(此时经度圈本就退化,矩形覆盖偏大
    // 只多收候选,不漏)。
    constexpr double kR = 6378137.0;
    const double cosLat = std::max(std::abs(std::cos(lat0)), 0.01);
    const double dLat = radiusMeters / kR;
    const double dLon = radiusMeters / (kR * cosLat);
    constexpr double kHalfPi = 1.5707963267948966;
    constexpr double kPi = 3.141592653589793;
    const double south = std::max(lat0 - dLat, -kHalfPi);
    const double north = std::min(lat0 + dLat, kHalfPi);
    // 跨反经线:Rectangle 约定 west>east 表示跨越,candidates 收集用
    // intersects 正确处理;经度跨度 ≥ 全圆时直接取全经度带。
    double west = lon0 - dLon;
    double east = lon0 + dLon;
    if (east - west >= 2.0 * kPi) {
        west = -kPi;
        east = kPi;
    } else {
        if (west < -kPi) west += 2.0 * kPi;
        if (east > kPi) east -= 2.0 * kPi;
    }
    const LoadedTerrainAreaSampler sampler =
        terrainTileset->createAreaHeightSampler(
            Rectangle(west, south, east, north));
    if (sampler.empty()) {
        return;
    }
    for (size_t i = 0; i < localOffsetsMeters.size(); ++i) {
        const glm::dvec2& off = localOffsetsMeters[i];
        double lat = lat0 + off.y / kR;
        lat = std::clamp(lat, -kHalfPi, kHalfPi);
        double lon = lon0 + off.x / (kR * cosLat);
        if (lon > kPi) lon -= 2.0 * kPi;
        if (lon < -kPi) lon += 2.0 * kPi;
        const std::optional<float> h = sampler.sample(lon, lat);
        if (!h) {
            continue;
        }
        out[i].valid = true;
        out[i].heightMeters = static_cast<double>(*h);
        out[i].surfaceEcef = ellipsoid.cartographicToCartesian(
            Cartographic(lon, lat, static_cast<double>(*h)));
    }
}

} // namespace earth_engine
