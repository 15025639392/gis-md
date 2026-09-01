#include "PolygonTessellator.h"

#include "ConstrainedDelaunay.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/math/Vec3.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace earth_engine {

namespace {

// 近重合点去重:量化到 ~1e-9 rad(赤道约 6mm)网格。编辑可能产生重合顶点,
// 而 CDT 前置约定点两两不同。
constexpr double kQuantum = 1e-9;

int64_t quantize(double v) {
    return static_cast<int64_t>(std::llround(v / kQuantum));
}

uint64_t coordKey(double lng, double lat) {
    // 两个 int64 折叠(足够避免实际数据碰撞)。
    const uint64_t a = static_cast<uint64_t>(quantize(lng));
    const uint64_t b = static_cast<uint64_t>(quantize(lat));
    return a * 1000003ull ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2));
}

uint64_t edgeIndexKey(uint32_t a, uint32_t b) {
    if (a > b) std::swap(a, b);
    return (static_cast<uint64_t>(a) << 32) | b;
}

constexpr int kMaxPiecesPerEdge = 256;
constexpr int kMaxInteriorSteiner = 4096;
constexpr double kEarthRadiusMeters = 6378137.0;

bool pointInRingsEvenOdd(double lng, double lat,
                         const std::vector<std::vector<Cartographic>>& rings) {
    bool inside = false;
    for (const auto& ring : rings) {
        const size_t n = ring.size();
        if (n < 3) continue;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const double yi = ring[i].latitude();
            const double yj = ring[j].latitude();
            if ((yi > lat) != (yj > lat)) {
                const double xAtY =
                    ring[j].longitude() +
                    (ring[i].longitude() - ring[j].longitude()) *
                        (lat - ring[j].latitude()) / (yi - yj);
                if (lng < xAtY) inside = !inside;
            }
        }
    }
    return inside;
}

} // namespace

TessellatedFill PolygonTessellator::tessellate(
    const Feature& feature,
    const Ellipsoid& ellipsoid,
    double heightOffset,
    const std::vector<Cartographic>* steinerPoints,
    double maxEdgeMeters,
    PolygonTessellationDiagnostics* diagnostics) {
    using Clock = std::chrono::steady_clock;
    auto elapsedMs = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start)
            .count();
    };
    const auto setupStart = Clock::now();
    TessellatedFill out;
    if (feature.type != GeometryType::Polygon || feature.rings.empty()) {
        return out;
    }

    // 全局去重:唯一点表 + (ring,vertex) → 唯一索引 remap。
    std::unordered_map<uint64_t, uint32_t> uniqueMap;
    std::vector<glm::dvec2> points2D;   // (lng,lat) 唯一点
    std::vector<Cartographic> uniqueCart;

    auto internUnique = [&](const Cartographic& c) -> uint32_t {
        const uint64_t key = coordKey(c.longitude(), c.latitude());
        auto it = uniqueMap.find(key);
        if (it != uniqueMap.end()) return it->second;
        const auto idx = static_cast<uint32_t>(points2D.size());
        uniqueMap.emplace(key, idx);
        points2D.emplace_back(c.longitude(), c.latitude());
        uniqueCart.push_back(c);
        return idx;
    };

    std::vector<ConstrainedDelaunay::Edge> constraints;
    std::vector<uint32_t> outline;

    for (const auto& ring : feature.rings) {
        if (ring.size() < 3) continue;
        // 环内 remap 到唯一索引;丢弃闭合末点(== 首点)与连续重复。
        std::vector<uint32_t> loop;
        loop.reserve(ring.size());
        for (const auto& c : ring) {
            const uint32_t ui = internUnique(c);
            if (!loop.empty() && loop.back() == ui) continue;  // 连续重复
            loop.push_back(ui);
        }
        // 闭合:若首尾同一唯一点,去掉尾。
        while (loop.size() >= 2 && loop.front() == loop.back()) loop.pop_back();
        if (loop.size() < 3) continue;

        // 约束边 + 描边:环的闭合边序列。
        for (size_t i = 0; i < loop.size(); ++i) {
            const uint32_t u = loop[i];
            const uint32_t v = loop[(i + 1) % loop.size()];
            if (u == v) continue;
            constraints.emplace_back(u, v);
            outline.push_back(u);
            outline.push_back(v);
        }
    }
    if (diagnostics) {
        diagnostics->inputPoints += points2D.size();
        diagnostics->initialConstraints += constraints.size();
        diagnostics->setupMs += elapsedMs(setupStart);
    }

    // 地球网格:约束边按椭球弦长切开,再在面内撒 Steiner。不做的话 CDT
    const auto densifyStart = Clock::now();
    // 对角线可以跨过整个水面/地块,斜视近裁就是射线。
    if (maxEdgeMeters > 0.0 && !constraints.empty()) {
        std::vector<ConstrainedDelaunay::Edge> split;
        std::vector<uint32_t> newOutline;
        split.reserve(constraints.size() * 2);
        newOutline.reserve(outline.size() * 2);
        for (const auto& e : constraints) {
            const Cartographic& ca = uniqueCart[e.first];
            const Cartographic& cb = uniqueCart[e.second];
            const Vec3 pa = ellipsoid.cartographicToCartesian(Cartographic(
                ca.longitude(), ca.latitude(), ca.height() + heightOffset));
            const Vec3 pb = ellipsoid.cartographicToCartesian(Cartographic(
                cb.longitude(), cb.latitude(), cb.height() + heightOffset));
            const double chord = (pb - pa).length();
            const int pieces = std::max(
                1, std::min(kMaxPiecesPerEdge,
                            static_cast<int>(std::ceil(chord / maxEdgeMeters))));
            uint32_t prev = e.first;
            for (int p = 1; p < pieces; ++p) {
                const double t = static_cast<double>(p) / static_cast<double>(pieces);
                const Vec3 mix = pa * (1.0 - t) + pb * t;
                const std::optional<Vec3> surf =
                    ellipsoid.tryScaleToGeodeticSurface(mix);
                if (!surf.has_value()) continue;
                const Cartographic cc = ellipsoid.cartesianToCartographic(*surf);
                const double h = ca.height() + (cb.height() - ca.height()) * t;
                const uint32_t id = internUnique(
                    Cartographic(cc.longitude(), cc.latitude(), h));
                if (id == prev) continue;
                split.emplace_back(prev, id);
                newOutline.push_back(prev);
                newOutline.push_back(id);
                prev = id;
            }
            if (e.second != prev) {
                split.emplace_back(prev, e.second);
                newOutline.push_back(prev);
                newOutline.push_back(e.second);
            }
        }
        constraints = std::move(split);
        outline = std::move(newOutline);
    }

    // 内部 Steiner 散点(P3 贴地):进唯一点表参与 CDT,无约束边。落在
    // 环外/与已有点重合的经 intern 去重与 flood-fill 自然无害。
    if (steinerPoints) {
        for (const Cartographic& c : *steinerPoints) internUnique(c);
    }

    if (maxEdgeMeters > 0.0 && !uniqueCart.empty()) {
        double west = uniqueCart[0].longitude();
        double east = uniqueCart[0].longitude();
        double south = uniqueCart[0].latitude();
        double north = uniqueCart[0].latitude();
        for (const Cartographic& c : uniqueCart) {
            west = std::min(west, c.longitude());
            east = std::max(east, c.longitude());
            south = std::min(south, c.latitude());
            north = std::max(north, c.latitude());
        }
        double step = maxEdgeMeters / kEarthRadiusMeters;
        const double spanLng = std::max(0.0, east - west);
        const double spanLat = std::max(0.0, north - south);
        // bbox 已经短于 maxEdge → 边也不会拆,内部再撒点只会把小建筑
        // 打成几十三角,host 测例/近景 footprint 都吃亏。
        int nx = static_cast<int>(std::floor(spanLng / step));
        int ny = static_cast<int>(std::floor(spanLat / step));
        if (nx >= 1 || ny >= 1) {
            nx = std::max(1, nx);
            ny = std::max(1, ny);
            if (nx * ny > kMaxInteriorSteiner) {
                const double scale = std::sqrt(
                    static_cast<double>(nx * ny) /
                    static_cast<double>(kMaxInteriorSteiner));
                step *= scale;
                nx = std::max(1, static_cast<int>(std::floor(spanLng / step)));
                ny = std::max(1, static_cast<int>(std::floor(spanLat / step)));
            }
            for (int iy = 0; iy < ny; ++iy) {
                const double lat = south + (iy + 0.5) * step;
                if (lat >= north) break;
                for (int ix = 0; ix < nx; ++ix) {
                    const double lng = west + (ix + 0.5) * step;
                    if (lng >= east) break;
                    if (!pointInRingsEvenOdd(lng, lat, feature.rings)) continue;
                    internUnique(Cartographic(lng, lat, 0.0));
                }
            }
        }
    }
    if (diagnostics) {
        diagnostics->densifiedPoints += points2D.size();
        diagnostics->globeDensifyMs += elapsedMs(densifyStart);
    }

    if (points2D.size() < 3 || constraints.empty()) return out;

    // ---- 约束边求交预分裂(编辑畸形输入) ----
    // 编辑把顶点拖过对边会产生自交环,而 CDT 前置约定约束边不交叉(违约
    // 输入会搅乱奇偶 flood-fill → fill 与 outline 对不上的"破碎多边形")。
    // 这里建立前置条件:对每对非共端点约束边求交——真交叉插入交点为
    // Steiner 点、端点落在对方边内(T 型接触)记为对方的分裂点、共线重叠
    // 按互相包含的端点分裂;逐边按参数排序重建子段。分裂后 flood-fill 的
    // 奇偶计数即 even-odd 填充语义(蝴蝶结两叶都填)。子段仍在原线段上,
    // 不产生新交叉,单趟即收敛。O(E²) 对编辑尺度(几十~几百边)可忽略。
    const auto intersectionStart = Clock::now();
    {
        // 交点的 Cartographic 沿边端点线性插值(容差尺度下误差可忽略)。
        auto internLerp = [&](const ConstrainedDelaunay::Edge& e,
                              double t) -> uint32_t {
            const Cartographic& a = uniqueCart[e.first];
            const Cartographic& b = uniqueCart[e.second];
            return internUnique(Cartographic(
                a.longitude() + (b.longitude() - a.longitude()) * t,
                a.latitude() + (b.latitude() - a.latitude()) * t,
                a.height() + (b.height() - a.height()) * t));
        };
        auto cross2 = [](const glm::dvec2& a, const glm::dvec2& b) {
            return a.x * b.y - a.y * b.x;
        };

        // 每条边的分裂点:(沿边参数 t, 唯一点索引)。
        std::vector<std::vector<std::pair<double, uint32_t>>> cuts(
            constraints.size());
        struct EdgeBounds {
            double minX;
            double minY;
            double maxX;
            double maxY;
        };
        std::vector<EdgeBounds> bounds;
        bounds.reserve(constraints.size());
        constexpr double kIntersectionBoundsPadding = 2.0 * kQuantum;
        for (const auto& edge : constraints) {
            const glm::dvec2 a = points2D[edge.first];
            const glm::dvec2 b = points2D[edge.second];
            bounds.push_back({
                std::min(a.x, b.x) - kIntersectionBoundsPadding,
                std::min(a.y, b.y) - kIntersectionBoundsPadding,
                std::max(a.x, b.x) + kIntersectionBoundsPadding,
                std::max(a.y, b.y) + kIntersectionBoundsPadding});
        }
        size_t pairCount = 0;
        size_t candidatePairCount = 0;
        for (size_t i = 0; i < constraints.size(); ++i) {
            const glm::dvec2 a1 = points2D[constraints[i].first];
            const glm::dvec2 a2 = points2D[constraints[i].second];
            const glm::dvec2 r = a2 - a1;
            const double rLen2 = glm::dot(r, r);
            if (rLen2 <= 0.0) continue;
            // 参数容差 = 量化格(kQuantum)换算到本边参数空间:交点离端点
            // 不足一个量化格 → 视为端点接触,不分裂。
            const double tEps = 2.0 * kQuantum / std::sqrt(rLen2);
            for (size_t j = i + 1; j < constraints.size(); ++j) {
                ++pairCount;
                const EdgeBounds& aBounds = bounds[i];
                const EdgeBounds& bBounds = bounds[j];
                if (aBounds.maxX < bBounds.minX ||
                    bBounds.maxX < aBounds.minX ||
                    aBounds.maxY < bBounds.minY ||
                    bBounds.maxY < aBounds.minY) {
                    continue;
                }
                ++candidatePairCount;
                const bool shareEndpoint =
                    constraints[i].first == constraints[j].first ||
                    constraints[i].first == constraints[j].second ||
                    constraints[i].second == constraints[j].first ||
                    constraints[i].second == constraints[j].second;
                const glm::dvec2 b1 = points2D[constraints[j].first];
                const glm::dvec2 b2 = points2D[constraints[j].second];
                const glm::dvec2 s = b2 - b1;
                const double sLen2 = glm::dot(s, s);
                if (sLen2 <= 0.0) continue;
                const double uEps = 2.0 * kQuantum / std::sqrt(sLen2);
                const glm::dvec2 d = b1 - a1;
                const double denom = cross2(r, s);
                // 近平行阈值:sin(夹角) 小于量化格/边长量级时走共线分支。
                const double parallelEps =
                    2.0 * kQuantum * std::sqrt(std::max(rLen2, sLen2));
                if (std::abs(denom) <= parallelEps) {
                    // 平行:仅共线重叠需要处理(把对方端点投影为分裂点)。
                    if (std::abs(cross2(d, r)) >
                        2.0 * kQuantum * std::sqrt(rLen2)) {
                        continue;  // 平行不共线
                    }
                    const uint32_t jEnds[2] = {constraints[j].first,
                                               constraints[j].second};
                    for (uint32_t pIdx : jEnds) {
                        const double t =
                            glm::dot(points2D[pIdx] - a1, r) / rLen2;
                        if (t > tEps && t < 1.0 - tEps) {
                            cuts[i].emplace_back(t, pIdx);
                        }
                    }
                    const uint32_t iEnds[2] = {constraints[i].first,
                                               constraints[i].second};
                    for (uint32_t pIdx : iEnds) {
                        const double u =
                            glm::dot(points2D[pIdx] - b1, s) / sLen2;
                        if (u > uEps && u < 1.0 - uEps) {
                            cuts[j].emplace_back(u, pIdx);
                        }
                    }
                    continue;
                }
                const double t = cross2(d, s) / denom;
                const double u = cross2(d, r) / denom;
                if (t < -tEps || t > 1.0 + tEps ||
                    u < -uEps || u > 1.0 + uEps) {
                    continue;  // 延长线相交,不在两线段上
                }
                const bool tInterior = t > tEps && t < 1.0 - tEps;
                const bool uInterior = u > uEps && u < 1.0 - uEps;
                if (tInterior && uInterior) {
                    // 真交叉(共端点边到不了这里):插 Steiner 点,两边都分裂
                    const uint32_t pIdx = internLerp(constraints[i], t);
                    cuts[i].emplace_back(t, pIdx);
                    cuts[j].emplace_back(u, pIdx);
                } else if (tInterior && !shareEndpoint) {
                    // j 的端点落在 i 内部(T 型):分裂 i
                    cuts[i].emplace_back(t, u < 0.5 ? constraints[j].first
                                                    : constraints[j].second);
                } else if (uInterior && !shareEndpoint) {
                    cuts[j].emplace_back(u, t < 0.5 ? constraints[i].first
                                                    : constraints[i].second);
                }
            }
        }

        std::vector<ConstrainedDelaunay::Edge> splitConstraints;
        splitConstraints.reserve(constraints.size());
        for (size_t i = 0; i < constraints.size(); ++i) {
            if (cuts[i].empty()) {
                splitConstraints.push_back(constraints[i]);
                continue;
            }
            std::sort(cuts[i].begin(), cuts[i].end());
            uint32_t prev = constraints[i].first;
            for (const auto& [t, idx] : cuts[i]) {
                if (idx != prev) {
                    splitConstraints.emplace_back(prev, idx);
                    prev = idx;
                }
            }
            if (constraints[i].second != prev) {
                splitConstraints.emplace_back(prev, constraints[i].second);
            }
        }
        constraints = std::move(splitConstraints);
        if (diagnostics) {
            diagnostics->intersectionPairs += pairCount;
            diagnostics->intersectionCandidatePairs += candidatePairCount;
        }
    }

    // The flood fill implements even-odd fill, so coincident constraint
    // segments must also be reduced modulo two. Amap compound polygons contain
    // adjacent strips that share exact or partially-overlapping edges; after
    // the intersection split above those sub-segments have identical endpoints.
    // Keeping one copy turns the shared seam into a wall and flips one side to
    // outside, which later appears as stable triangular holes/wedges.
    {
        struct CountedEdge {
            ConstrainedDelaunay::Edge edge;
            uint32_t count = 0;
        };
        std::unordered_map<uint64_t, CountedEdge> counts;
        counts.reserve(constraints.size());
        for (const auto& edge : constraints) {
            const uint64_t key = edgeIndexKey(edge.first, edge.second);
            auto [it, inserted] = counts.emplace(key, CountedEdge{edge, 0});
            ++it->second.count;
        }
        std::vector<ConstrainedDelaunay::Edge> oddConstraints;
        oddConstraints.reserve(counts.size());
        for (const auto& [key, counted] : counts) {
            if ((counted.count & 1u) != 0u) {
                oddConstraints.push_back(counted.edge);
            }
        }
        constraints = std::move(oddConstraints);
    }
    if (diagnostics) {
        diagnostics->intersectionConstraints += constraints.size();
        diagnostics->intersectionMs += elapsedMs(intersectionStart);
    }

    const auto cdtStart = Clock::now();
    ConstrainedDelaunayDiagnostics cdtDiagnostics;
    std::vector<uint32_t> tris =
        ConstrainedDelaunay::triangulate(
            points2D, constraints, diagnostics ? &cdtDiagnostics : nullptr);
    if (diagnostics) {
        diagnostics->triangleCount += tris.size() / 3;
        diagnostics->cdtMs += elapsedMs(cdtStart);
        diagnostics->cdtSuperTriangleMs += cdtDiagnostics.superTriangleMs;
        diagnostics->cdtPointInsertMs += cdtDiagnostics.pointInsertMs;
        diagnostics->cdtConstraintInsertMs +=
            cdtDiagnostics.constraintInsertMs;
        diagnostics->cdtExtractInsideMs += cdtDiagnostics.extractInsideMs;
        diagnostics->cdtPointTriangleTests +=
            cdtDiagnostics.pointTriangleTests;
        diagnostics->cdtPointBadTriangles += cdtDiagnostics.pointBadTriangles;
        diagnostics->cdtConstraintEdgeTests +=
            cdtDiagnostics.constraintEdgeLookups;
        diagnostics->cdtConstraintCrossTests +=
            cdtDiagnostics.constraintCrossTriangleTests;
        diagnostics->cdtConstraintsAlreadyPresent +=
            cdtDiagnostics.constraintsAlreadyPresent;
        diagnostics->cdtConstraintsInserted +=
            cdtDiagnostics.constraintsInserted;
        diagnostics->cdtPeakTriangles = std::max(
            diagnostics->cdtPeakTriangles, cdtDiagnostics.peakTriangles);
        diagnostics->cdtPointCapacityGrowths +=
            cdtDiagnostics.pointCapacityGrowths;
        diagnostics->cdtTriangleCapacityGrowths +=
            cdtDiagnostics.triangleCapacityGrowths;
    }
    if (tris.empty()) return out;

    // 唯一点 → ECEF 顶点。
    const auto ecefStart = Clock::now();
    out.positions.reserve(uniqueCart.size());
    for (const auto& c : uniqueCart) {
        Cartographic ch(c.longitude(), c.latitude(), c.height() + heightOffset);
        out.positions.push_back(ellipsoid.cartographicToCartesian(ch));
    }
    out.fillIndices = std::move(tris);
    out.outlineIndices = std::move(outline);
    if (diagnostics) diagnostics->ecefMs += elapsedMs(ecefStart);
    return out;
}

} // namespace earth_engine
