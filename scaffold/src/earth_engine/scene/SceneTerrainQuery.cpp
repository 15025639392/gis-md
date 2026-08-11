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
    std::vector<CameraSystem::TerrainSample>& out) {
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
    // cos(lat) 放大,极区钳到下限防除零。旧实现按矩形预收候选瓦片,统一
    // 采样服务的 cell 索引让逐点查询本身 O(档数),预筛不再必要。
    constexpr double kR = 6378137.0;
    const double cosLat = std::max(std::abs(std::cos(lat0)), 0.01);
    constexpr double kHalfPi = 1.5707963267948966;
    constexpr double kPi = 3.141592653589793;
    (void)radiusMeters;
    const TerrainHeightService& heights = terrainTileset->heightService();
    for (size_t i = 0; i < localOffsetsMeters.size(); ++i) {
        const glm::dvec2& off = localOffsetsMeters[i];
        double lat = lat0 + off.y / kR;
        lat = std::clamp(lat, -kHalfPi, kHalfPi);
        double lon = lon0 + off.x / (kR * cosLat);
        if (lon > kPi) lon -= 2.0 * kPi;
        if (lon < -kPi) lon += 2.0 * kPi;
        // 相机碰撞要的是"不穿上屏那张面" → 渲染网格一致采样。
        const auto h = heights.sample(
            lon, lat, TerrainHeightService::Interp::RenderGridConsistent);
        if (!h) {
            continue;
        }
        out[i].valid = true;
        out[i].heightMeters = static_cast<double>(h->height);
        out[i].surfaceEcef = ellipsoid.cartographicToCartesian(
            Cartographic(lon, lat, static_cast<double>(h->height)));
    }
}

} // namespace earth_engine
