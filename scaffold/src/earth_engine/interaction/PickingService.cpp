#include "PickingService.h"
#include "../scene/Camera.h"
#include "../layers/VectorLayer.h"
#include "../data/GeoJsonParser.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/math/Ray.h"

#include <glm/glm.hpp>
#include <cmath>
#include <limits>
#include <algorithm>

namespace earth_engine {

// ============================================================
// 射线-椭球相交
// ============================================================

Vec3 PickingService::rayEllipsoidIntersection(const Vec3& origin,
                                                const Vec3& direction) {
    const auto& e = Ellipsoid::WGS84();
    double a = e.semiMajorAxis();
    double b = e.semiMinorAxis();
    double s = a / b;  // z 轴缩放因子

    // 缩放到单位球空间
    double ox = origin.x();
    double oy = origin.y();
    double oz = origin.z() * s;
    double dx = direction.x();
    double dy = direction.y();
    double dz = direction.z() * s;

    // Ray-sphere: |o + t*d|² = a²
    double A = dx * dx + dy * dy + dz * dz;
    double B = 2.0 * (ox * dx + oy * dy + oz * dz);
    double C = ox * ox + oy * oy + oz * oz - a * a;

    double discriminant = B * B - 4.0 * A * C;
    if (discriminant < 0.0) return Vec3::zero();

    double sqrtD = std::sqrt(discriminant);
    double t0 = (-B - sqrtD) / (2.0 * A);
    double t1 = (-B + sqrtD) / (2.0 * A);

    // 取最小的正 t
    double t = -1.0;
    if (t0 > 0.0) t = t0;
    else if (t1 > 0.0) t = t1;

    if (t <= 0.0) return Vec3::zero();

    return Vec3(
        origin.x() + t * direction.x(),
        origin.y() + t * direction.y(),
        origin.z() + t * direction.z()
    );
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
    constexpr double kEpsilon = 1e-8;

    Vec3 edge1(v1.x() - v0.x(), v1.y() - v0.y(), v1.z() - v0.z());
    Vec3 edge2(v2.x() - v0.x(), v2.y() - v0.y(), v2.z() - v0.z());

    Vec3 h(rayDirection.y() * edge2.z() - rayDirection.z() * edge2.y(),
           rayDirection.z() * edge2.x() - rayDirection.x() * edge2.z(),
           rayDirection.x() * edge2.y() - rayDirection.y() * edge2.x());

    double a = edge1.x() * h.x() + edge1.y() * h.y() + edge1.z() * h.z();
    if (std::abs(a) < kEpsilon) return false; // parallel

    double f = 1.0 / a;
    Vec3 s(rayOrigin.x() - v0.x(),
           rayOrigin.y() - v0.y(),
           rayOrigin.z() - v0.z());

    double u = f * (s.x() * h.x() + s.y() * h.y() + s.z() * h.z());
    if (u < 0.0 || u > 1.0) return false;

    Vec3 q(s.y() * edge1.z() - s.z() * edge1.y(),
           s.z() * edge1.x() - s.x() * edge1.z(),
           s.x() * edge1.y() - s.y() * edge1.x());

    double v = f * (rayDirection.x() * q.x() +
                    rayDirection.y() * q.y() +
                    rayDirection.z() * q.z());
    if (v < 0.0 || u + v > 1.0) return false;

    t = f * (edge2.x() * q.x() + edge2.y() * q.y() + edge2.z() * q.z());
    return t > kEpsilon;
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

    Vec3 hit = rayEllipsoidIntersection(ray.origin(), ray.direction());

    if (hit.x() != 0.0 || hit.y() != 0.0 || hit.z() != 0.0) {
        result.hitType = PickResult::HitType::Ellipsoid;
        result.worldPosition = hit;
        result.cartographic =
            Ellipsoid::WGS84().cartesianToCartographic(hit);
        result.distance = (hit - ray.origin()).length();
    }

    return result;
}

PickResult PickingService::pickTerrain(
    float screenXPixels, float screenYPixels,
    const Camera& camera,
    double viewportWidthPixels, double viewportHeightPixels,
    std::function<float(double,double)> terrainSampler) const {

    // 先获取椭球命中
    PickResult result = pickEllipsoid(screenXPixels, screenYPixels,
                                       camera, viewportWidthPixels,
                                       viewportHeightPixels);

    // 如果命中椭球且有地形采样器，查询高度
    if (result.hitType == PickResult::HitType::Ellipsoid && terrainSampler) {
        float h = terrainSampler(
            static_cast<float>(result.cartographic.longitude()),
            static_cast<float>(result.cartographic.latitude()));

        if (h != 0.0f) {
            result.hitType = PickResult::HitType::Terrain;
            result.terrainHeight = h;

            // 更新世界位置：沿法线位移
            const auto& e = Ellipsoid::WGS84();
            auto surfacePt = e.cartographicToCartesian(result.cartographic);
            auto normal = e.geodeticSurfaceNormal(result.cartographic);
            result.worldPosition = Vec3(
                surfacePt.x() + normal.x() * static_cast<double>(h),
                surfacePt.y() + normal.y() * static_cast<double>(h),
                surfacePt.z() + normal.z() * static_cast<double>(h));
        }
    }

    return result;
}

} // namespace earth_engine
