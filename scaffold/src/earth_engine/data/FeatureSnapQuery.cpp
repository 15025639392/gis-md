#include "FeatureSnapQuery.h"

#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/Vec3.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace earth_engine {

namespace {

/// 点到线段最近点(ECEF 直线段近似;容差尺度远小于地表曲率半径)。
Vec3 closestPointOnSegment(const Vec3& p, const Vec3& a, const Vec3& b,
                           double& outT) {
    const Vec3 ab = b - a;
    const double len2 = ab.x() * ab.x() + ab.y() * ab.y() + ab.z() * ab.z();
    double t = 0.0;
    if (len2 > 0.0) {
        const Vec3 ap = p - a;
        t = (ap.x() * ab.x() + ap.y() * ab.y() + ap.z() * ab.z()) / len2;
        t = std::clamp(t, 0.0, 1.0);
    }
    outT = t;
    return a + ab * t;
}

} // namespace

std::optional<SnapCandidate> FeatureSnapQuery::nearest(
    const FeatureStore& store,
    const Ellipsoid& ellipsoid,
    const Cartographic& around,
    double toleranceMeters,
    FeatureId exclude) {
    if (store.empty() || toleranceMeters <= 0.0) return std::nullopt;

    // R-tree 预筛:容差米 → 弧度 bbox(纬度余弦修正,极区钳制)。
    const double dLat = toleranceMeters / 6.378137e6;
    const double cosLat = std::max(0.01, std::cos(around.latitude()));
    const double dLng = dLat / cosLat;
    const Rectangle bbox(around.longitude() - dLng,
                         around.latitude() - dLat,
                         around.longitude() + dLng,
                         around.latitude() + dLat);
    std::vector<FeatureId> candidates = store.queryVisible(bbox);
    if (candidates.empty()) return std::nullopt;
    std::sort(candidates.begin(), candidates.end());  // 结果确定性

    const Vec3 refEcef = ellipsoid.cartographicToCartesian(around);

    std::optional<SnapCandidate> bestVertex;
    double bestVertexDist = toleranceMeters;
    std::optional<SnapCandidate> bestEdge;
    double bestEdgeDist = toleranceMeters;

    for (FeatureId fid : candidates) {
        if (fid == exclude) continue;
        const Feature* feature = store.getFeature(fid);
        if (!feature) continue;

        for (size_t r = 0; r < feature->rings.size(); ++r) {
            const auto& ring = feature->rings[r];
            const size_t n = ring.size();
            std::vector<Vec3> ecef;
            ecef.reserve(n);
            for (const auto& c : ring) {
                ecef.push_back(ellipsoid.cartographicToCartesian(c));
            }
            // 顶点
            for (size_t i = 0; i < n; ++i) {
                const double d = (ecef[i] - refEcef).length();
                if (d <= bestVertexDist) {
                    bestVertexDist = d;
                    SnapCandidate c;
                    c.featureId = fid;
                    c.part = SnapCandidate::Part::Vertex;
                    c.ringIndex = static_cast<int>(r);
                    c.vertexIndex = static_cast<int>(i);
                    c.position = ring[i];
                    c.distanceMeters = d;
                    bestVertex = c;
                }
            }
            // 边(polygon 环含闭合末边;LineString 不闭合;Point 无边)
            if (feature->type == GeometryType::Point || n < 2) continue;
            const size_t edgeCount =
                feature->type == GeometryType::Polygon ? n : n - 1;
            for (size_t i = 0; i < edgeCount; ++i) {
                const size_t j = (i + 1) % n;
                double t = 0.0;
                const Vec3 closest =
                    closestPointOnSegment(refEcef, ecef[i], ecef[j], t);
                const double d = (closest - refEcef).length();
                if (d <= bestEdgeDist) {
                    bestEdgeDist = d;
                    const Cartographic& a = ring[i];
                    const Cartographic& b = ring[j];
                    SnapCandidate c;
                    c.featureId = fid;
                    c.part = SnapCandidate::Part::Edge;
                    c.ringIndex = static_cast<int>(r);
                    c.vertexIndex = static_cast<int>(i);
                    c.position = Cartographic(
                        a.longitude() + (b.longitude() - a.longitude()) * t,
                        a.latitude() + (b.latitude() - a.latitude()) * t,
                        a.height() + (b.height() - a.height()) * t);
                    c.distanceMeters = d;
                    bestEdge = c;
                }
            }
        }
    }

    if (bestVertex) return bestVertex;  // 顶点优先
    return bestEdge;
}

} // namespace earth_engine
