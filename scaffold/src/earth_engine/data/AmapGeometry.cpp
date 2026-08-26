#include "AmapGeometry.h"

#include "../core/geodesy/Gcj02CoordinateTransform.h"
#include "../core/geodesy/Ellipsoid.h"
#include "PolygonTessellator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace earth_engine {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;

/// 环有向面积(鞋带公式);符号编码绕向。
double ringSignedArea(const std::vector<std::pair<double, double>>& ring) {
    double sum = 0.0;
    const size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        sum += (ring[j].first + ring[i].first) *
               (ring[j].second - ring[i].second);
    }
    return sum / 2.0;
}

/// 射线法点在环内测试(用于 even-odd 嵌套深度)。
bool pointInRing(const std::vector<std::pair<double, double>>& ring,
                 double px, double py) {
    bool inside = false;
    const size_t n = ring.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = ring[i].first, yi = ring[i].second;
        const double xj = ring[j].first, yj = ring[j].second;
        if (((yi > py) != (yj > py)) &&
            (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }
    return inside;
}

struct RingBounds {
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
};

RingBounds ringBounds(const std::vector<std::pair<double, double>>& ring) {
    RingBounds b;
    bool first = true;
    for (const auto& p : ring) {
        if (first) {
            b.minX = b.maxX = p.first;
            b.minY = b.maxY = p.second;
            first = false;
        } else {
            b.minX = std::min(b.minX, p.first);
            b.maxX = std::max(b.maxX, p.first);
            b.minY = std::min(b.minY, p.second);
            b.maxY = std::max(b.maxY, p.second);
        }
    }
    return b;
}

/// 环内严格内点:水平扫描线最宽内部跨度的中点。嵌套判定必须用内点而非
/// 边界顶点 —— 掩膜的相邻条带共享精确边,条带自己的顶点会被误判成相邻
/// 条带内(参考实现注释:spurious unfilled streak)。多高度试,规避扫描线
/// 擦过顶点(奇/零穿越)。
std::optional<std::pair<double, double>> ringInteriorPoint(
    const std::vector<std::pair<double, double>>& ring,
    const RingBounds& bounds) {
    if (ring.size() < 3) return std::nullopt;
    const double spanY = bounds.maxY - bounds.minY;
    const double span = std::max({bounds.maxX - bounds.minX, spanY, 1.0});
    const double eps = 1e-12 * span;
    if (spanY <= eps || bounds.maxX - bounds.minX <= eps) {
        return std::nullopt;
    }
    for (const double frac : {0.5, 0.37, 0.63, 0.24, 0.76}) {
        const double y = bounds.minY + spanY * frac;
        std::vector<double> xs;
        const size_t n = ring.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const double yi = ring[i].second, yj = ring[j].second;
            if ((yi > y) != (yj > y)) {
                xs.push_back(ring[j].first +
                             (ring[i].first - ring[j].first) *
                                 (y - yj) / (yi - yj));
            }
        }
        if (xs.size() >= 2 && (xs.size() % 2) == 0) {
            std::sort(xs.begin(), xs.end());
            double bestMid = 0.0;
            double bestW = -1.0;
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                const double w = xs[k + 1] - xs[k];
                if (w > bestW) {
                    bestW = w;
                    bestMid = (xs[k] + xs[k + 1]) / 2.0;
                }
            }
            if (bestW > eps && pointInRing(ring, bestMid, y)) {
                return std::make_pair(bestMid, y);
            }
        }
    }
    return std::nullopt;
}

/// 先在完整 even-odd 多边形上做 CDT,再逐三角形裁剪,最后从三角形并集
/// 恢复边界环。
///
/// 直接对凹环做 Sutherland–Hodgman 只能返回一个 ring；当窗口把一个
/// 凹面切成多个离散分量时,那个 ring 会包含跨分量的隐式桥边,后续 CDT
/// 会把桥边填成大楔形。三角形是单连通凸面,逐个裁剪不会产生该拓扑
/// 歧义。边计数再把内部共享边消掉,因此对外仍只产生正常的 polygon
/// rings,不会把每个三角形暴露成一个渲染 Feature。
std::vector<std::vector<std::pair<double, double>>>
triangulateThenClipPolygon(
    const std::vector<std::vector<std::pair<double, double>>>& rings,
    double minX, double maxX, double minY, double maxY) {
    // Reuse the engine tessellator's constraint intersection splitting and
    // even-odd flood fill. A tiny local radian scale keeps the planar tile
    // coordinates numerically stable while cartesianToCartographic recovers
    // the same local x/y for the clipped output.
    constexpr double kLocalRadians = 1e-5;
    Feature polygon;
    polygon.type = GeometryType::Polygon;
    for (const auto& ring : rings) {
        std::vector<Cartographic> local;
        local.reserve(ring.size());
        for (const auto& p : ring) {
            local.emplace_back(p.first * kLocalRadians,
                               p.second * kLocalRadians, 0.0);
        }
        polygon.rings.push_back(std::move(local));
    }
    const Ellipsoid& ellipsoid = Ellipsoid::WGS84();
    const TessellatedFill full =
        PolygonTessellator::tessellate(polygon, ellipsoid);
    if (full.fillIndices.empty()) return {};
    std::vector<glm::dvec2> points;
    points.reserve(full.positions.size());
    for (const auto& position : full.positions) {
        const Cartographic c = ellipsoid.cartesianToCartographic(position);
        points.emplace_back(c.longitude() / kLocalRadians,
                            c.latitude() / kLocalRadians);
    }
    const std::vector<uint32_t>& tris = full.fillIndices;
    if (tris.empty()) return {};

    // Keep the clipped convex pieces as rings. They can be aggregated into a
    // single Feature: PolygonTessellator cancels duplicated internal edges
    // modulo two, so no fragile boundary-loop reconstruction is needed at
    // T-junctions or around holes/disconnected components.
    std::vector<std::vector<std::pair<double, double>>> loops;
    loops.reserve(tris.size() / 3);
    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        const auto& a = points[tris[i]];
        const auto& b = points[tris[i + 1]];
        const auto& c = points[tris[i + 2]];
        std::vector<std::pair<double, double>> triangle = {
            {a.x, a.y}, {b.x, b.y}, {c.x, c.y}};
        auto clipped = amapClipPolygonRing(triangle, minX, maxX,
                                            minY, maxY);
        if (clipped.size() < 3) continue;
        if (ringSignedArea(clipped) < 0.0) {
            std::reverse(clipped.begin(), clipped.end());
        }
        loops.push_back(std::move(clipped));
    }
    return loops;
}

}  // namespace

bool amapRegionUsesLineGrid(int regionKind) {
    return regionKind == 60 || regionKind == 64 || regionKind == 80;
}

double amapCoordScale(int layerType, int layerZ, int regionKind) {
    // POI label anchors use a dedicated 2048×1024 grid at every data zoom
    // except z3.  They are not line geometry: reusing the z14 line scale (2)
    // compresses every label into half a tile and can place it in unrelated
    // water.  Keep this contract at the shared conversion boundary so both
    // the live POI source and direct geometry tests use the same rule.
    if (layerType == 0) return layerZ <= 3 ? 8.0 : 4.0;
    if (layerType == 3) return 8192.0 / 131072.0;  // 建筑 1/16
    if (layerType == 2) {
        // 普通区域恒 2048×1024(任意 zoom);大区域 kind 60/80 走 line-grid。
        if (amapRegionUsesLineGrid(regionKind)) {
            if (layerZ >= 14) return 2.0;
            if (layerZ >= 6) return 4.0;
            return 8.0;
        }
        return 4.0;
    }
    if (layerZ >= 14) return 2.0;                  // 线/轨道 z14+ 4096×2048
    if (layerZ >= 6) return 4.0;
    return 8.0;
}

Cartographic amapTileLocalToLngLat(int tileX, int tileY, int z,
                                   double localX, double localY,
                                   bool flipY) {
    const double n = std::exp2(z);
    const double lonDeg = (tileX + localX / 8192.0) / n * 360.0 - 180.0;
    const double latDeg =
        flipY ? 90.0 - (tileY + localY / 4096.0) / n * 180.0
              : (tileY + localY / 4096.0) / n * 180.0 - 90.0;
    return Cartographic(lonDeg * kDegToRad, latDeg * kDegToRad, 0.0);
}

/// Amap geometry blobs are encoded in a bottom-up local grid, while
/// `amapTileLocalToLngLat` and the reference implementation consume the
/// canonical top-down grid (y=0 at the tile's north edge).  Keep this
/// conversion at the decoder boundary so every geometry type uses the same
/// Y contract instead of accidentally mirroring only some layers.
double amapRawLocalYToTopDown(double rawY, double scale) {
    constexpr double kAmapExtentY = 4096.0;
    return kAmapExtentY - rawY * scale;
}

std::vector<std::vector<std::pair<double, double>>> amapNormalizeEvenOddWinding(
    const std::vector<std::vector<std::pair<double, double>>>& rings) {
    const size_t n = rings.size();

    // 高德 geometry blob 的 ring 可以省略重复首点,但语义仍是 polygon
    // 闭环；这里保持点列不变,面积/包含判定和后续裁剪都按隐式
    // last→first 边处理。不要把 protobuf 分帧误当成首点,也不要在裁剪后
    // 额外沿窗口补角,否则会改变同一个 clipped polygon 的填充区域。
    std::vector<std::vector<std::pair<double, double>>> closed = rings;
    if (n <= 1) return closed;

    std::vector<double> areas(n);
    std::vector<double> absAreas(n);
    std::vector<RingBounds> bounds(n);
    std::vector<std::optional<std::pair<double, double>>> interior(n);
    for (size_t i = 0; i < n; ++i) {
        areas[i] = ringSignedArea(closed[i]);
        absAreas[i] = std::abs(areas[i]);
        bounds[i] = ringBounds(closed[i]);
        interior[i] = ringInteriorPoint(closed[i], bounds[i]);
    }

    // 每个环的 even-odd 嵌套深度 + 最近严格包含它的环。
    std::vector<int> depth(n, 0);
    std::vector<int> parent(n, -1);
    for (size_t i = 0; i < n; ++i) {
        int d = 0;
        int best = -1;
        double bestArea = std::numeric_limits<double>::max();
        if (!interior[i]) continue;
        const double px = interior[i]->first, py = interior[i]->second;
        for (size_t j = 0; j < n; ++j) {
            if (j == i || absAreas[j] <= absAreas[i]) continue;
            if (px < bounds[j].minX || px > bounds[j].maxX ||
                py < bounds[j].minY || py > bounds[j].maxY) {
                continue;
            }
            if (pointInRing(closed[j], px, py)) {
                ++d;
                if (absAreas[j] < bestArea) {
                    bestArea = absAreas[j];
                    best = static_cast<int>(j);
                }
            }
        }
        depth[i] = d;
        parent[i] = best;
    }

    // 无被包围环时保持参考解码器行为；独立负绕向环会在 Feature 分组
    // 阶段被识别并反转为正向外环。
    bool hasEnclosed = false;
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 1) {
            hasEnclosed = true;
            break;
        }
    }
    if (!hasEnclosed) return closed;

    auto oriented = [&](size_t i, bool wantPositive)
        -> std::vector<std::pair<double, double>> {
        if ((areas[i] > 0.0) == wantPositive) return closed[i];
        std::vector<std::pair<double, double>> rev = closed[i];
        std::reverse(rev.begin(), rev.end());
        return rev;
    };

    std::vector<std::vector<std::pair<double, double>>> out;
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 != 0) continue;  // 外环只从偶数深度出
        out.push_back(oriented(i, true));  // 外环 → area > 0
        for (size_t j = 0; j < n; ++j) {
            if (depth[j] % 2 == 1 && parent[j] == static_cast<int>(i)) {
                out.push_back(oriented(j, false));  // 孔 → area < 0
            }
        }
    }
    // 嵌套分析退化时绝不丢环：没有可靠 parent 的负绕向环按独立外环
    // 输出并统一为正向，避免后续 nonzero fill 把它抵消掉。
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 1 && parent[i] == -1) {
            out.push_back(oriented(i, true));
        }
    }
    return out;
}

std::vector<std::pair<double, double>> amapClipPolygonRing(
    const std::vector<std::pair<double, double>>& ring,
    double minX, double maxX, double minY, double maxY) {
    // Sutherland–Hodgman:逐轴对齐半平面裁剪,交点解析计算(参考
    // xinzhi-map clipPolygonRing 的 lerp 求交)。
    auto lerp = [](const auto& a, const auto& b, double t) {
        return std::make_pair(a.first + (b.first - a.first) * t,
                              a.second + (b.second - a.second) * t);
    };
    auto clipHalfPlane =
        [&](const std::vector<std::pair<double, double>>& in,
            const std::function<bool(double, double)>& keep,
            const std::function<std::pair<double, double>(
                const std::pair<double, double>&,
                const std::pair<double, double>&)>& intersect) {
            if (in.empty()) return in;
            std::vector<std::pair<double, double>> out;
            auto prev = in.back();
            bool prevIn = keep(prev.first, prev.second);
            for (const auto& cur : in) {
                const bool curIn = keep(cur.first, cur.second);
                if (curIn) {
                    if (!prevIn) out.push_back(intersect(prev, cur));
                    out.push_back(cur);
                } else if (prevIn) {
                    out.push_back(intersect(prev, cur));
                }
                prev = cur;
                prevIn = curIn;
            }
            return out;
        };
    auto r = ring;
    // left: x >= minX
    r = clipHalfPlane(
        r,
        [&](double x, double) { return x >= minX; },
        [&](const auto& a, const auto& b) {
            return lerp(a, b, (minX - a.first) / (b.first - a.first));
        });
    // right: x <= maxX
    r = clipHalfPlane(
        r,
        [&](double x, double) { return x <= maxX; },
        [&](const auto& a, const auto& b) {
            return lerp(a, b, (maxX - a.first) / (b.first - a.first));
        });
    // bottom: y >= minY
    r = clipHalfPlane(
        r,
        [&](double, double y) { return y >= minY; },
        [&](const auto& a, const auto& b) {
            return lerp(a, b, (minY - a.second) / (b.second - a.second));
        });
    // top: y <= maxY
    r = clipHalfPlane(
        r,
        [&](double, double y) { return y <= maxY; },
        [&](const auto& a, const auto& b) {
            return lerp(a, b, (maxY - a.second) / (b.second - a.second));
        });
    return r.size() >= 3 ? r : std::vector<std::pair<double, double>>{};
}

std::vector<Feature> amapDecodedPartToFeatures(
    const AmapDecodedLayerPart& part, bool toWgs84) {
    std::vector<Feature> out;
    for (const auto& f : part.features) {
        const bool isRegion = part.type == 2 || f.polygonGeometry;
        const bool isLine = !f.polygonGeometry &&
                            (part.type == 1 || part.type == 4);
        // 区域按 kind 决定 scale(60/64/80 走 line-grid);
        // 其余类型 scale 与 kind 无关。
        // type2 content.#2 boundary lines use the line-grid scale even though
        // they remain in a type2 layer container.
        const int geometryLayerType =
            f.lineGeometry ? 1 : (f.polygonGeometry ? 2 : part.type);
        const double scale =
            f.coordScale > 0.0
                ? f.coordScale
                : amapCoordScale(geometryLayerType, part.z, f.kind);
        // type 0:POI 点标签。anchor = 单点 plain unsigned(2048×1024 空间,
        // scale 4),转 Point Feature。
        if (part.type == 0) {
            for (const auto& ring : f.rings) {
                if (ring.empty()) continue;
                Feature feat;
                feat.type = GeometryType::Point;
                const auto& pt = ring[0];
                Cartographic c = amapTileLocalToLngLat(
                    part.x, part.y, part.z, pt.first * scale,
                    amapRawLocalYToTopDown(pt.second, scale));
                if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                feat.rings = {{{c}}};
                feat.properties["amap_class"] = std::to_string(f.classCode);
                feat.properties["amap_type"] = std::to_string(part.type);
                feat.properties["amap_subkey"] = std::to_string(f.subKey);
                feat.properties["amap_rank"] = std::to_string(f.rank);
                feat.properties["amap_minzoom"] = std::to_string(f.minZoom);
                feat.properties["amap_maxzoom"] = std::to_string(f.maxZoom);
                // FeatureRenderLayer sorts ascending (smaller = earlier),
                // while Amap rank uses larger = more important.  Negate at
                // the adapter boundary so the per-tile symbol budget keeps
                // Amap's important labels first.
                feat.properties["rank"] = std::to_string(-f.rank);
                if (!f.name.empty()) {
                    feat.properties["name"] = f.name;
                }
                out.push_back(std::move(feat));
            }
            continue;
        }
        if (isLine || f.lineGeometry) {
            for (const auto& ring : f.rings) {
                Feature feat;
                feat.type = GeometryType::LineString;
                std::vector<Cartographic> pts;
                pts.reserve(ring.size());
                for (const auto& pt : ring) {
                    Cartographic c = amapTileLocalToLngLat(
                        part.x, part.y, part.z, pt.first * scale,
                        amapRawLocalYToTopDown(pt.second, scale));
                    if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                    pts.push_back(c);
                }
                feat.rings.push_back(std::move(pts));
                feat.properties["amap_class"] = std::to_string(f.classCode);
                feat.properties["amap_type"] = std::to_string(part.type);
                if (f.kind > 0) {
                    feat.properties["amap_kind"] = std::to_string(f.kind);
                }
                out.push_back(std::move(feat));
            }
            continue;
        }

        // type3 建筑:footprint 外环+庭院孔,非 even-odd 掩膜。每个 ring
        // 独立 polygon(建筑带孔少见,且裁剪会破坏孔);补闭合即可。
        if (part.type == 3) {
            for (const auto& ring : f.rings) {
                Feature feat;
                feat.type = GeometryType::Polygon;
                std::vector<Cartographic> pts;
                pts.reserve(ring.size());
                for (const auto& pt : ring) {
                    Cartographic c = amapTileLocalToLngLat(
                        part.x, part.y, part.z, pt.first * scale,
                        amapRawLocalYToTopDown(pt.second, scale));
                    if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                    pts.push_back(c);
                }
                if (pts.size() >= 3 &&
                    !(pts.front().longitude() == pts.back().longitude() &&
                      pts.front().latitude() == pts.back().latitude())) {
                    pts.push_back(pts.front());
                }
                feat.rings.push_back(std::move(pts));
                feat.properties["amap_class"] = std::to_string(f.classCode);
                feat.properties["amap_type"] = std::to_string(part.type);
                if (f.kind > 0) {
                    feat.properties["amap_kind"] = std::to_string(f.kind);
                }
                if (f.height > 0.0) {
                    feat.properties["amap_height"] =
                        std::to_string(f.height);
                }
                out.push_back(std::move(feat));
            }
            continue;
        }

        // type2 与 type4 content.#3 区域:环全部同向(even-odd 掩膜),归一化为
        // 「外环 + 孔环」分组,每组一个 Polygon(三角化自动挖孔)。
        // 归一化保证:外环 area>0,孔 area<0,且每个外环后紧跟它的孔
        // (下一个外环之前)。消费端按绕向符号分组即可,无需重算嵌套。
        //
        // ⚠️ 归一化必须与 CDT/经纬度转换使用同一 canonical top-down
        // 坐标语义：raw 高德 Y 自南向北，先翻成 y=0 在北。
        // 归一化的面积/绕向若不翻转,与三角化坐标符号相反 → 外环/孔
        // 分组颠倒,CDT 挖错区域(蓝色过度填充)。参考实现 loadGeometry
        // 先 `AMAP_EXTENT_Y - y*scale` 翻转再做 even-odd 归一化,随后
        // amap_reprojected_tile.js 才做瓦片裁剪 —— 顺序:翻转→归一化
        // →裁剪(先裁剪会破坏嵌套判定)。
        constexpr double kAmapExtentY = 4096.0;
        auto flippedRings = f.rings;
        for (auto& ring : flippedRings) {
            for (auto& pt : ring) {
                // 参考实现 loadGeometry:先乘 scale 进 canonical,再翻转 y。
                pt.first *= scale;
                pt.second = kAmapExtentY - pt.second * scale;
            }
        }
        const auto groups = amapNormalizeEvenOddWinding(flippedRings);
        bool touchesTileBoundary = false;
        bool hasOpenRing = false;
        for (const auto& ring : flippedRings) {
            if (ring.size() >= 2 && ring.front() != ring.back()) {
                hasOpenRing = true;
            }
            for (const auto& pt : ring) {
                touchesTileBoundary =
                    touchesTileBoundary || pt.first <= 0.0 ||
                    pt.first >= 8192.0 || pt.second <= 0.0 ||
                    pt.second >= 4096.0;
            }
        }

        // Small ordinary surfaces retain the compact outer+hole representation.
        // Complex compound masks (multiple clipped components/strips) use the
        // triangle-piece path below; it preserves even-odd coverage without
        // inventing a bridge when clipping splits a concave ring.
        if (isRegion && f.kind > 0 && f.rings.size() <= 2) {
            // Kind surfaces already normalize into outer-followed-by-holes
            // groups. Preserve those groups directly; the tessellator's
            // modulo-two constraint handling resolves their shared seams
            // without the extra triangulate/union pass used by legacy kind=0
            // compound masks.
            for (size_t gi = 0; gi < groups.size();) {
                if (ringSignedArea(groups[gi]) <= 0.0) {
                    ++gi;
                    continue;
                }
                Feature feat;
                feat.type = GeometryType::Polygon;
                for (size_t ri = gi; ri < groups.size(); ++ri) {
                    if (ri > gi && ringSignedArea(groups[ri]) > 0.0) break;
                    const auto clipped = amapClipPolygonRing(
                        groups[ri], -256.0, 8192.0 + 256.0,
                        -256.0, 4096.0 + 256.0);
                    if (clipped.size() < 3) continue;
                    std::vector<Cartographic> pts;
                    pts.reserve(clipped.size());
                    for (const auto& pt : clipped) {
                        Cartographic c = amapTileLocalToLngLat(
                            part.x, part.y, part.z, pt.first,
                            pt.second);
                        if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                        pts.push_back(std::move(c));
                    }
                    feat.rings.push_back(std::move(pts));
                }
                if (!feat.rings.empty()) {
                    feat.properties["amap_class"] =
                        std::to_string(f.classCode);
                    feat.properties["amap_type"] = std::to_string(part.type);
                    feat.properties["amap_kind"] = std::to_string(f.kind);
                    feat.properties["amap_subkey"] =
                        std::to_string(f.subKey);
                    feat.properties["amap_fillkey"] =
                        std::to_string(f.classCode) + ":" +
                        std::to_string(f.kind);
                    out.push_back(std::move(feat));
                }
                while (gi < groups.size()) {
                    ++gi;
                    if (gi < groups.size() && ringSignedArea(groups[gi]) > 0.0) {
                        break;
                    }
                }
            }
            continue;
        }

        if (isRegion &&
            (f.rings.size() > 2 ||
             (f.kind == 0 && touchesTileBoundary && hasOpenRing))) {
            // 先对原始 even-odd 约束整体 CDT,再对每个凸三角形做窗口
            // 裁剪。这样凹面与窗口相交产生多个分量时不会生成隐式桥边。
            const auto clippedLoops = triangulateThenClipPolygon(
                flippedRings, -256.0, 8192.0 + 256.0,
                -256.0, 4096.0 + 256.0);
            auto toCarto = [&](const std::vector<std::pair<double, double>>& ring) {
                std::vector<Cartographic> pts;
                pts.reserve(ring.size());
                for (const auto& pt : ring) {
                    Cartographic c = amapTileLocalToLngLat(
                        part.x, part.y, part.z, pt.first,
                        pt.second);
                    if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                    pts.push_back(std::move(c));
                }
                return pts;
            };
            auto setProperties = [&](Feature& feat) {
                feat.properties["amap_class"] = std::to_string(f.classCode);
                feat.properties["amap_type"] = std::to_string(part.type);
                if (f.kind > 0) {
                    feat.properties["amap_kind"] = std::to_string(f.kind);
                }
                feat.properties["amap_subkey"] = std::to_string(f.subKey);
                feat.properties["amap_fillkey"] =
                    std::to_string(f.classCode) + ":" +
                    (f.kind > 0 ? std::to_string(f.kind) : "0");
            };
            // Keep triangles belonging to the same connected component in one
            // Feature. Shared triangle edges cancel modulo two inside the
            // tessellator, while disconnected islands remain separate
            // Features (the public decoder contract and style batching).
            struct PointKey {
                int64_t x = 0;
                int64_t y = 0;
                bool operator==(const PointKey& other) const {
                    return x == other.x && y == other.y;
                }
            };
            struct PointHash {
                size_t operator()(const PointKey& p) const {
                    return static_cast<size_t>(
                        static_cast<uint64_t>(p.x) * 0x9e3779b97f4a7c15ull ^
                        (static_cast<uint64_t>(p.y) + 0x85ebca6bull));
                }
            };
            constexpr double kPointQuantum = 1e-7;
            std::unordered_map<PointKey, uint32_t, PointHash> pointIds;
            uint32_t nextPointId = 0;
            auto pointId = [&](const std::pair<double, double>& p) {
                PointKey key{static_cast<int64_t>(std::llround(
                                 p.first / kPointQuantum)),
                             static_cast<int64_t>(std::llround(
                                 p.second / kPointQuantum))};
                auto [it, inserted] = pointIds.emplace(key, nextPointId);
                if (inserted) ++nextPointId;
                return it->second;
            };
            auto edgeKey = [](uint32_t a, uint32_t b) {
                if (a > b) std::swap(a, b);
                return (static_cast<uint64_t>(a) << 32) | b;
            };
            std::unordered_map<uint64_t, std::vector<size_t>> edgeOwners;
            for (size_t ri = 0; ri < clippedLoops.size(); ++ri) {
                const auto& ring = clippedLoops[ri];
                for (size_t pi = 0; pi < ring.size(); ++pi) {
                    const uint32_t a = pointId(ring[pi]);
                    const uint32_t b = pointId(ring[(pi + 1) % ring.size()]);
                    edgeOwners[edgeKey(a, b)].push_back(ri);
                }
            }
            std::vector<std::vector<size_t>> adjacency(clippedLoops.size());
            for (const auto& [key, owners] : edgeOwners) {
                for (size_t i = 1; i < owners.size(); ++i) {
                    adjacency[owners[0]].push_back(owners[i]);
                    adjacency[owners[i]].push_back(owners[0]);
                }
            }
            std::vector<uint8_t> visited(clippedLoops.size(), 0);
            for (size_t start = 0; start < clippedLoops.size(); ++start) {
                if (visited[start]) continue;
                std::vector<size_t> component;
                std::vector<size_t> stack = {start};
                visited[start] = 1;
                while (!stack.empty()) {
                    const size_t current = stack.back();
                    stack.pop_back();
                    component.push_back(current);
                    for (const size_t next : adjacency[current]) {
                        if (!visited[next]) {
                            visited[next] = 1;
                            stack.push_back(next);
                        }
                    }
                }
                Feature feat;
                feat.type = GeometryType::Polygon;
                for (const size_t index : component) {
                    auto carto = toCarto(clippedLoops[index]);
                    if (carto.size() >= 3) feat.rings.push_back(std::move(carto));
                }
                if (!feat.rings.empty()) {
                    setProperties(feat);
                    out.push_back(std::move(feat));
                }
            }
            continue;
        }


        const auto toCarto = [&](const auto& ring) {
            std::vector<Cartographic> pts;
            // 裁剪窗口 = ±256 buffer([-256,8448]×[-256,4352]),与参考
            // 实现一致。Sutherland–Hodgman 按隐式 last→first 边裁剪；
            // 裁剪结果直接交给 PolygonTessellator/VectorTileRasterizer，
            // 两者都按 modulo 隐式闭合。这里不能再沿窗口边界补一条
            // “候选路径”，否则会把同一个 clipped polygon 改成互补区域。
            std::vector<std::pair<double, double>> src;
            if (isRegion) {
                const auto clipped =
                    amapClipPolygonRing(ring, -256.0, 8192.0 + 256.0,
                                        -256.0, 4096.0 + 256.0);
                if (clipped.size() < 3) return pts;  // 窗外无幸存
                src = clipped;
            } else {
                src = ring;
            }
            pts.reserve(src.size());
            for (const auto& pt : src) {
                // 归一化/裁剪已经在 canonical top-down 坐标中；直接传给
                // amapTileLocalToLngLat。这里若再做 4096-y 会把每块瓦片
                // 内部上下镜像，并在相邻 tile row 之间形成水平条带错位。
                Cartographic c = amapTileLocalToLngLat(
                    part.x, part.y, part.z, pt.first,
                    pt.second);
                if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                pts.push_back(c);
            }
            return pts;
        };
        std::vector<std::vector<Cartographic>> pending;  // 外环 + 孔
        auto flush = [&]() {
            if (pending.empty()) return;
            Feature feat;
            feat.type = GeometryType::Polygon;
            for (auto& ring : pending) feat.rings.push_back(std::move(ring));
            pending.clear();
            feat.properties["amap_class"] = std::to_string(f.classCode);
            feat.properties["amap_type"] = std::to_string(part.type);
            if (f.kind > 0) {
                feat.properties["amap_kind"] = std::to_string(f.kind);
            }
            // regionBlocks(30002)按 subKey 逐用地类型上色(@xinzhi/amap-style
            // colors.regionBlocks);subKey 缺省 1 → 兜底 $block。30001 仍按
            // kind(61 绿地 / 63 水系 / 15 海洋)。
            feat.properties["amap_subkey"] = std::to_string(f.subKey);
            // fill 配色分流键:classCode 区分数据层(30001 水/绿地、
            // 30002 地块),kind 是层内细分。合成单键供 StyleExpression
            // match(它只支持单属性匹配)。
            feat.properties["amap_fillkey"] =
                std::to_string(f.classCode) + ":" +
                (f.kind > 0 ? std::to_string(f.kind) : "0");
            if (part.type == 3 && f.height > 0.0) {
                feat.properties["amap_height"] = std::to_string(f.height);
            }
            out.push_back(std::move(feat));
        };
        for (const auto& ring : groups) {
            // 裁剪后不足三个点的退化环丢弃。若它是外环,先结束当前分组
            // (避免后续孔错误并入)。
            auto carto = toCarto(ring);
            if (carto.empty()) {
                const double a = ringSignedArea(ring);
                if (a > 0.0) flush();
                continue;
            }
            const double area = ringSignedArea(ring);
            if (area > 0.0) {
                flush();  // 新外环 → 结束上一分组
                pending.push_back(std::move(carto));
            } else {
                // 负面积环:仅当**被当前外环包含**才是孔(参考 mapbox
                // classifyRings:孔必须在外环内)。互不嵌套的独立碎片
                // 即使绕向为负也应是独立外环 —— 旧实现一律并入前一个
                // 外环,CDT 在碎片之间大面积填充(大量错误三角形根因)。
                // 用环内点(面积符号无关)做包含测试:取当前环内一点,
                // 判断它是否在 pending 外环内。
                bool insideOuter = false;
                if (!pending.empty()) {
                    const auto& outer = pending.front();
                    std::vector<std::pair<double, double>> outer2;
                    outer2.reserve(outer.size());
                    for (const auto& c : outer) {
                        outer2.emplace_back(c.longitude(), c.latitude());
                    }
                    std::vector<std::pair<double, double>> hole2;
                    hole2.reserve(carto.size());
                    for (const auto& c : carto) {
                        hole2.emplace_back(c.longitude(), c.latitude());
                    }
                    const auto holeBounds = ringBounds(hole2);
                    const auto holePoint = ringInteriorPoint(hole2, holeBounds);
                    // 孔自身的严格内点必须在当前外环内；bbox 中心可能
                    // 落在凹环外，不能用于这个判定。
                    if (holePoint) {
                        insideOuter = pointInRing(
                            outer2, holePoint->first, holePoint->second);
                    }
                }
                if (insideOuter) {
                    pending.push_back(std::move(carto));  // 孔,加入当前外环
                } else {
                    // 独立碎片(负绕向但互不嵌套):作为独立外环，并改为
                    // 正绕向，避免 nonzero raster fill 与其他外环相消。
                    flush();
                    std::reverse(carto.begin(), carto.end());
                    pending.push_back(std::move(carto));
                }
            }
        }
        flush();
    }
    return out;
}

bool amapBytesToFeatures(const uint8_t* data, size_t size,
                         bool regionsOnly, std::vector<Feature>& out,
                         std::string* error) {
    std::vector<AmapDecodedLayerPart> parts;
    if (!decodeAmapTile(data, size, parts, error)) {
        return false;
    }
    for (const auto& p : parts) {
        if (p.type == 2) {
            if (!regionsOnly) {
                // 主源只保留 30002 等城市地块面；30001 水/绿地由
                // z12 water source 唯一提供。即使样式把 30001 设透明，
                // 在解码阶段过滤仍可避免 z14 错位水体进入 tessellation/
                // GPU bucket，杜绝与 z12 水层的重复工作和潜在双带。
                AmapDecodedLayerPart kept = p;
                kept.features.clear();
                for (const auto& f : p.features) {
                    if (f.classCode != 30001) kept.features.push_back(f);
                }
                auto fs = amapDecodedPartToFeatures(kept, false);
                out.insert(out.end(), std::make_move_iterator(fs.begin()),
                           std::make_move_iterator(fs.end()));
                continue;
            }
            // 粗源(z10/z12)保留全部 type2；样式按 class/kind 选择水、
            // 绿地和地块颜色。
            auto fs = amapDecodedPartToFeatures(p, false);
            out.insert(out.end(), std::make_move_iterator(fs.begin()),
                       std::make_move_iterator(fs.end()));
            continue;
        }
        // type1 线 / type3 建筑 / type4 轨道:主源才要。
        if (regionsOnly) continue;
        auto fs = amapDecodedPartToFeatures(p, false);
        out.insert(out.end(), std::make_move_iterator(fs.begin()),
                   std::make_move_iterator(fs.end()));
    }
    return true;
}

}  // namespace earth_engine
