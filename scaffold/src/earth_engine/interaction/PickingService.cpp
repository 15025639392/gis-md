#include "PickingService.h"
#include "../scene/Camera.h"
#include "../layers/VectorLayer.h"
#include "../data/GeoJsonParser.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/math/IntersectionTests.h"
#include "../core/math/Ray.h"

#include <glm/glm.hpp>
#include <cmath>
#include <limits>
#include <algorithm>

namespace earth_engine {

// ============================================================
// 射线-椭球相交
// ============================================================

std::optional<Vec3> PickingService::rayEllipsoidIntersection(
    const Vec3& origin,
    const Vec3& direction) {
    const auto& e = Ellipsoid::WGS84();
    auto interval = e.rayIntersectionInterval(origin, direction);
    if (!interval) {
        return std::nullopt;
    }

    double t = interval->entryDistance;
    if (t <= 0.0) {
        auto originCartographic = e.tryCartesianToCartographic(origin);
        if (originCartographic &&
            std::abs(originCartographic->height()) < 1e-6) {
            return origin;
        }
        t = interval->exitDistance;
    }

    if (t <= 0.0) {
        return std::nullopt;
    }

    return origin + direction * t;
}

// ============================================================
// 射线-三角形相交（Möller–Trumbore）
// ============================================================

bool PickingService::rayTriangleIntersection(const Vec3& rayOrigin,
                                               const Vec3& rayDirection,
                                               const Vec3& v0,
                                               const Vec3& v1,
                                               const Vec3& v2,
                                               double& t) {
    const std::optional<double> hitT =
        IntersectionTests::rayTriangleParametric(
            Ray(rayOrigin, rayDirection),
            v0,
            v1,
            v2,
            false);
    if (!hitT || *hitT < 0.0) {
        return false;
    }
    t = *hitT;
    return true;
}

// ============================================================
// 拾取
// ============================================================

PickResult PickingService::pick(
    float screenXPixels, float screenYPixels,
    const Camera& camera,
    double viewportWidthPixels, double viewportHeightPixels,
    const std::vector<const VectorLayer*>& vectorLayers) const {

    // 先生成椭球命中作为 fallback
    PickResult best = pickEllipsoid(screenXPixels, screenYPixels,
                                     camera, viewportWidthPixels,
                                     viewportHeightPixels);

    Ray ray = camera.getPickRay(
        static_cast<double>(screenXPixels),
        static_cast<double>(screenYPixels),
        viewportWidthPixels, viewportHeightPixels);

    const Vec3& rayOrigin = ray.origin();
    const Vec3& rayDir = ray.direction();

    // 对每个矢量图层的 features 做射线-三角形相交
    for (const auto* layer : vectorLayers) {
        if (!layer || !layer->visible()) continue;

        for (const auto& feature : layer->features()) {
            if (feature.rings.empty()) continue;

            const auto& e = Ellipsoid::WGS84();

            if (feature.type == GeoFeature::Type::Polygon) {
                if (feature.rings[0].size() < 3) continue;

                // 转换 feature 顶点到 ECEF
                std::vector<Vec3> ecefVerts;
                for (const auto& c : feature.rings[0]) {
                    ecefVerts.push_back(e.cartographicToCartesian(c));
                }

                // Fan triangulation around first vertex
                for (size_t i = 1; i + 1 < ecefVerts.size(); ++i) {
                    double t = 0.0;
                    if (rayTriangleIntersection(rayOrigin, rayDir,
                        ecefVerts[0], ecefVerts[i], ecefVerts[i + 1], t)) {

                        Vec3 hitPt(
                            rayOrigin.x() + t * rayDir.x(),
                            rayOrigin.y() + t * rayDir.y(),
                            rayOrigin.z() + t * rayDir.z());

                        double dist = (hitPt - rayOrigin).length();

                        // 替换最近的
                        if (!best.isValid() || dist < best.distance) {
                            best.hitType = PickResult::HitType::VectorFeature;
                            best.layerId = layer->id();
                            best.featureId = feature.id;
                            best.distance = dist;
                            best.worldPosition = hitPt;
                            best.cartographic =
                                e.cartesianToCartographic(hitPt);
                            best.screenX = screenXPixels;
                            best.screenY = screenYPixels;
                        }
                    }
                }
            } else if (feature.type == GeoFeature::Type::LineString) {
                const auto& ring = feature.rings[0];
                if (ring.size() < 2) continue;

                // 动态容差：相机距离 → 每像素世界米数 × 10
                double camDist = rayOrigin.length();
                double pixelSizeWorld = camDist * std::tan(
                    camera.verticalFovRadians() * 0.5) /
                    (viewportHeightPixels * 0.5);
                double linePickTolerance = pixelSizeWorld * 10.0;
                double toleranceSq = linePickTolerance * linePickTolerance;

                // 预计算 |D|²（射线方向可能非单位长度）
                double a_coef = rayDir.x() * rayDir.x() +
                                rayDir.y() * rayDir.y() +
                                rayDir.z() * rayDir.z();

                for (size_t i = 0; i + 1 < ring.size(); ++i) {
                    Vec3 segA = e.cartographicToCartesian(ring[i]);
                    Vec3 segB = e.cartographicToCartesian(ring[i + 1]);

                    // 射线-线段最短距离（David Eberly 公式的约束变体）
                    double Ex = segB.x() - segA.x();
                    double Ey = segB.y() - segA.y();
                    double Ez = segB.z() - segA.z();
                    double Fx = segA.x() - rayOrigin.x();
                    double Fy = segA.y() - rayOrigin.y();
                    double Fz = segA.z() - rayOrigin.z();

                    double b_coef = rayDir.x() * Ex + rayDir.y() * Ey +
                                    rayDir.z() * Ez;
                    double c_coef = Ex * Ex + Ey * Ey + Ez * Ez;
                    double d_coef = rayDir.x() * Fx + rayDir.y() * Fy +
                                    rayDir.z() * Fz;
                    double e_coef = Ex * Fx + Ey * Fy + Ez * Fz;

                    double denom = a_coef * c_coef - b_coef * b_coef;
                    double t, u;

                    constexpr double kEpsSeg = 1e-12;
                    if (std::abs(denom) < kEpsSeg) {
                        // 平行：线段上最接近射线原点的点
                        u = (std::abs(b_coef) > kEpsSeg)
                            ? std::clamp(-d_coef / b_coef, 0.0, 1.0)
                            : 0.0;
                        double px = segA.x() + u * Ex;
                        double py = segA.y() + u * Ey;
                        double pz = segA.z() + u * Ez;
                        double tx = px - rayOrigin.x();
                        double ty = py - rayOrigin.y();
                        double tz = pz - rayOrigin.z();
                        t = std::max(0.0, (rayDir.x() * tx + rayDir.y() * ty +
                                           rayDir.z() * tz) / a_coef);
                    } else {
                        double t_unclamped = (b_coef * e_coef -
                                              c_coef * d_coef) / denom;
                        double u_unclamped = (a_coef * e_coef -
                                              b_coef * d_coef) / denom;

                        if (t_unclamped >= 0.0 &&
                            u_unclamped >= 0.0 && u_unclamped <= 1.0) {
                            t = t_unclamped;
                            u = u_unclamped;
                        } else {
                            // 约束 u → 重新计算 t
                            u = std::clamp(u_unclamped, 0.0, 1.0);
                            double px = segA.x() + u * Ex;
                            double py = segA.y() + u * Ey;
                            double pz = segA.z() + u * Ez;
                            double tx = px - rayOrigin.x();
                            double ty = py - rayOrigin.y();
                            double tz = pz - rayOrigin.z();
                            t = std::max(0.0,
                                (rayDir.x() * tx + rayDir.y() * ty +
                                 rayDir.z() * tz) / a_coef);
                        }
                    }

                    // 计算最近点对和距离
                    double rpx = rayOrigin.x() + t * rayDir.x();
                    double rpy = rayOrigin.y() + t * rayDir.y();
                    double rpz = rayOrigin.z() + t * rayDir.z();
                    double spx = segA.x() + u * Ex;
                    double spy = segA.y() + u * Ey;
                    double spz = segA.z() + u * Ez;

                    double dx = rpx - spx;
                    double dy = rpy - spy;
                    double dz = rpz - spz;
                    double distSq = dx * dx + dy * dy + dz * dz;

                    if (distSq < toleranceSq) {
                        double dist = std::sqrt(distSq);
                        if (!best.isValid() || dist < best.distance) {
                            best.hitType = PickResult::HitType::VectorFeature;
                            best.layerId = layer->id();
                            best.featureId = feature.id;
                            best.distance = dist;
                            best.worldPosition = Vec3(spx, spy, spz);
                            best.cartographic =
                                e.cartesianToCartographic(
                                    Vec3(spx, spy, spz));
                            best.screenX = screenXPixels;
                            best.screenY = screenYPixels;
                        }
                    }
                }
            }
        }
    }

    return best;
}

PickResult PickingService::pickEllipsoid(
    float screenXPixels, float screenYPixels,
    const Camera& camera,
    double viewportWidthPixels, double viewportHeightPixels) const {

    PickResult result;
    result.screenX = screenXPixels;
    result.screenY = screenYPixels;

    Ray ray = camera.getPickRay(
        static_cast<double>(screenXPixels),
        static_cast<double>(screenYPixels),
        viewportWidthPixels, viewportHeightPixels);

    auto hit = rayEllipsoidIntersection(ray.origin(), ray.direction());

    if (hit) {
        auto cartographic = Ellipsoid::WGS84().tryCartesianToCartographic(*hit);
        if (!cartographic) {
            return result;
        }
        result.hitType = PickResult::HitType::Ellipsoid;
        result.worldPosition = *hit;
        result.cartographic = *cartographic;
        result.distance = (*hit - ray.origin()).length();
    }

    return result;
}

PickResult PickingService::pickTerrain(
    float screenXPixels, float screenYPixels,
    const Camera& camera,
    double viewportWidthPixels, double viewportHeightPixels,
    std::function<float(double,double)> terrainSampler) const {

    // 无地形采样器时退化为纯椭球拾取。
    if (!terrainSampler) {
        return pickEllipsoid(screenXPixels, screenYPixels, camera,
                             viewportWidthPixels, viewportHeightPixels);
    }

    const auto& e = Ellipsoid::WGS84();
    const Ray ray = camera.getPickRay(
        static_cast<double>(screenXPixels),
        static_cast<double>(screenYPixels),
        viewportWidthPixels, viewportHeightPixels);
    PickResult result;
    result.screenX = screenXPixels;
    result.screenY = screenYPixels;

    // 射线碰不到椭球 ⇒ 也碰不到地形(地形在椭球之外):返回 None,与旧行为一致
    // (仰视天空/山体高于相机的仰角射线拿不到锚点,控制器回退抓取球/转台)。
    const std::optional<RayEllipsoidIntersectionInterval> interval =
        e.rayIntersectionInterval(ray.origin(), ray.direction());
    if (!interval) {
        return result;
    }
    const double tEntry = interval->entryDistance;

    // 自适应步长:相机越高,远处地形细节在像面上越不敏感。步长取相机海拔的
    // 1/4,钳在 [10m, 800m]:AGL≈600m 时 ≈150m,配合二分精修对崖面/坡面足够;
    // AGL≈50m 时 ≈12m。地形交点的用途是起手锚点/点选,一次手势只算一次,
    // 不落渲染循环。
    const double camH = std::max(
        e.cartesianToCartographic(ray.origin()).height(), 0.0);
    double step = std::clamp(0.25 * camH, 10.0, 800.0);

    auto terrainHeightAt = [&](double t) -> float {
        const Cartographic c = e.cartesianToCartographic(ray.pointAt(t));
        return terrainSampler(c.longitude(), c.latitude());
    };
    auto belowTerrain = [&](double t) -> bool {
        const Cartographic c = e.cartesianToCartographic(ray.pointAt(t));
        return c.height() < static_cast<double>(terrainHeightAt(t)) - 0.5;
    };

    // 起手即在地形内(仅防御;正常相机受钳位约束不会进入):返回眼位点。
    if (belowTerrain(0.0)) {
        result.hitType = PickResult::HitType::Terrain;
        result.worldPosition = ray.origin();
        result.cartographic = e.cartesianToCartographic(ray.origin());
        result.terrainHeight = terrainHeightAt(0.0);
        result.distance = 0.0;
        return result;
    }

    // 从相机向外行进,找第一个 rayHeight < terrainHeight 的区间再二分精修。
    // 地形在椭球之上,首个交点必在 [0, tEntry] 内;未命中(越过所有高地落到
    // 椭球)时退化为椭球入口点——平地/海面与旧行为一致,且不会返回"山后抬升
    // 点"(低空锚点贴眼/增益崩塌 P1 的根源)。
    double t = 0.0;
    double tPrev = 0.0;
    constexpr int kMaxMarchSamples = 96;
    for (int guard = 0; guard < kMaxMarchSamples; ++guard) {
        tPrev = t;
        t = std::min(t + step, tEntry);
        if (belowTerrain(t)) {
            double lo = tPrev;
            double hi = t;
            for (int i = 0; i < 10; ++i) {
                const double mid = 0.5 * (lo + hi);
                if (belowTerrain(mid)) {
                    hi = mid;
                } else {
                    lo = mid;
                }
            }
            const double tc = 0.5 * (lo + hi);
            const Vec3 hit = ray.pointAt(tc);
            result.hitType = PickResult::HitType::Terrain;
            result.worldPosition = hit;
            result.cartographic = e.cartesianToCartographic(hit);
            result.terrainHeight = terrainHeightAt(tc);
            result.distance = tc;
            return result;
        }
        if (t >= tEntry) {
            break;
        }
        step = std::min(step * 1.6, std::max(2000.0, 0.5 * (tEntry - t)));
    }

    // 未与地形相交:返回椭球入口点(该点地形≈海平面,见 belowTerrain 判据)。
    const Vec3 entry = ray.pointAt(tEntry);
    result.hitType = PickResult::HitType::Terrain;
    result.worldPosition = entry;
    result.cartographic = e.cartesianToCartographic(entry);
    result.terrainHeight = terrainHeightAt(tEntry);
    result.distance = tEntry;
    return result;
}

} // namespace earth_engine
