#include "AmapGeometry.h"
#include "AmapVectorSourceInternal.h"

#include "../core/geodesy/Gcj02CoordinateTransform.h"
#include "../core/geodesy/Ellipsoid.h"
#include "../core/geodesy/WebMercatorProjection.h"
#include "PolygonTessellator.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace earth_engine {

#if defined(EARTH_ENGINE_TESTING)
bool AmapDecodedTileDecodeTraits::decode(
    const uint8_t* data, size_t size, AmapDecodedTile& out,
    std::string* error) {
    AmapClassicSourceBundle::Impl::DecodedTile tile;
    const bool ok = AmapClassicSourceBundle::Impl::decodeType1(
        data, size, tile, error);
    out.parts = std::move(tile.parts);
    return ok;
}

size_t AmapDecodedTileDecodeTraits::approxBytes(
    const AmapDecodedTile& tile) {
    constexpr size_t kVectorHeaderBytes = sizeof(std::vector<int>);
    size_t bytes = sizeof(AmapDecodedTile) +
                   tile.parts.capacity() * sizeof(AmapDecodedLayerPart);
    for (const AmapDecodedLayerPart& part : tile.parts) {
        bytes += part.features.capacity() * sizeof(AmapDecodedFeature);
        for (const AmapDecodedFeature& feature : part.features) {
            bytes += feature.name.capacity();
            bytes += feature.rings.capacity() * kVectorHeaderBytes;
            for (const auto& ring : feature.rings) {
                bytes += ring.capacity() * sizeof(std::pair<double, double>);
            }
        }
    }
    return bytes;
}
#endif

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

bool amapBuildingResolutionIsValid(int layerZ, int buildingResolution) {
    if (layerZ < 0 || layerZ > 30 || buildingResolution < 2) return false;
    const int coordShift = 33 - buildingResolution - layerZ;
    return coordShift >= 0 && coordShift <= 31;
}

double amapCoordScale(int layerType, int layerZ, int regionKind,
                      int buildingResolution) {
    // POI label anchors use a dedicated 2048×1024 grid at every data zoom
    // except z3.  They are not line geometry: reusing the z14 line scale (2)
    // compresses every label into half a tile and can place it in unrelated
    // water.  Keep this contract at the shared conversion boundary so both
    // the live POI source and direct geometry tests use the same rule.
    if (layerType == 0) return layerZ <= 3 ? 8.0 : 4.0;
    if (layerType == 3) {
        if (!amapBuildingResolutionIsValid(layerZ, buildingResolution)) {
            return 0.0;
        }
        // resolution 12 -> 2048×1024; resolution 18 -> 131072×65536.
        // Canonical space is 8192×4096, hence 2^(14-resolution).
        return std::ldexp(1.0, 14 - buildingResolution);
    }
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
                                   double localX, double localY) {
    const double n = std::exp2(z);
    const double lonDeg = (tileX + localX / 8192.0) / n * 360.0 - 180.0;
    const double latDeg =
        90.0 - (tileY + localY / 4096.0) / n * 180.0;
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

std::optional<Cartographic> amapOfficialRoadShieldAnchor(
    int tileX, int tileY, int tileZ, double scale,
    const std::vector<std::pair<double, double>>& ring) {
    if (ring.size() < 2) return std::nullopt;
    if ((ring.size() & 1u) != 0u) {
        const auto& point = ring[ring.size() / 2];
        return amapTileLocalToLngLat(
            tileX, tileY, tileZ, point.first * scale,
            amapRawLocalYToTopDown(point.second, scale));
    }

    // Official handlerTileRoadLines first projects every real point into
    // EPSG:3857, optionally subtracts its fixed 128x128 LCS-cell center, and
    // only then lets NebulaLabelFormat.oV select the two central flat-array
    // scalars.  With 2k points that position is [Y(k-1), Xk].  Rebuild the
    // same fixed world anchor here, then immediately return to Cartographic
    // so terrain, ECEF, collision and GPU placement remain one shared path.
    const size_t right = ring.size() / 2;
    const size_t left = right - 1;
    const Cartographic leftCartographic = amapTileLocalToLngLat(
        tileX, tileY, tileZ, ring[left].first * scale,
        amapRawLocalYToTopDown(ring[left].second, scale));
    const Cartographic rightCartographic = amapTileLocalToLngLat(
        tileX, tileY, tileZ, ring[right].first * scale,
        amapRawLocalYToTopDown(ring[right].second, scale));
    const WebMercatorProjection projection(Ellipsoid::WGS84());
    const Vec3 leftProjected = projection.project(leftCartographic);
    const Vec3 rightProjected = projection.project(rightCartographic);

    double anchorX = leftProjected.y();
    double anchorY = rightProjected.x();
    if (tileZ >= 13) {
        constexpr double kWorldHalf = 20037508.342789244;
        constexpr double kLcsCellsPerAxis = 128.0;
        constexpr double kLcsCellSize =
            (2.0 * kWorldHalf) / kLcsCellsPerAxis;
        // NebulaTileCoord.ga publishes [west,north,east,south], and
        // handlerTile selects [Ro[0],Ro[1]].  The LCS frame therefore belongs
        // to the projected north-west tile corner, not the south-west corner.
        const Cartographic tileNorthWest = amapTileLocalToLngLat(
            tileX, tileY, tileZ, 0.0, 0.0);
        const Vec3 tileNorthWestProjected =
            projection.project(tileNorthWest);
        const double centerX =
            (std::floor(tileNorthWestProjected.x() / kLcsCellSize) + 0.5) *
            kLcsCellSize;
        const double centerY =
            (std::floor(tileNorthWestProjected.y() / kLcsCellSize) + 0.5) *
            kLcsCellSize;
        anchorX = centerX + leftProjected.y() - centerY;
        anchorY = centerY + rightProjected.x() - centerX;
    }
    if (!std::isfinite(anchorX) || !std::isfinite(anchorY)) {
        return std::nullopt;
    }
    return projection.unproject(glm::dvec2(anchorX, anchorY));
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

    // 无被包围环时，每个环都是独立的外环。方向对 even-odd 掩膜本身
    // 没有语义，但消费端会把负面积环误当成前一个外环的孔；统一为正向
    // 可让后续分组保持一环一个 polygon，避免把互不相交的碎片合并填满。
    bool hasEnclosed = false;
    for (size_t i = 0; i < n; ++i) {
        if (depth[i] % 2 == 1) {
            hasEnclosed = true;
            break;
        }
    }
    if (!hasEnclosed) {
        std::vector<std::vector<std::pair<double, double>>> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (areas[i] >= 0.0) {
                out.push_back(closed[i]);
            } else {
                auto ring = closed[i];
                std::reverse(ring.begin(), ring.end());
                out.push_back(std::move(ring));
            }
        }
        return out;
    }

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

std::vector<Feature> AmapClassicSourceBundle::Impl::convertPart(
    const AmapDecodedLayerPart& part, bool toWgs84,
    bool (*lineIdentityFilter)(int, int),
    bool (*polygonIdentityFilter)(int, int),
    bool (*pointIdentityFilter)(int, int)) {
    std::vector<Feature> out;
    for (const auto& f : part.features) {
        auto publishOfficialWindow = [&](Feature& feature) {
            if (f.hasMinZoom) {
                feature.properties["amap_minzoom"] =
                    std::to_string(f.minZoom);
            }
            if (f.hasMaxZoom) {
                feature.properties["amap_maxzoom"] =
                    std::to_string(f.maxZoom);
            }
        };
        // PoiLayer(type 0) is point-only by contract. Keep the explicit
        // semantic bit for mixed type-4 containers, but fail closed when a
        // hand-built/test feature omits it.
        const bool isPoint = f.pointGeometry || part.type == 0;
        const bool isRegion = part.type == 2 || f.polygonGeometry;
        const bool isLine = !isPoint && !f.polygonGeometry &&
                            (part.type == 1 || part.type == 4);
        if (isPoint && pointIdentityFilter &&
            !pointIdentityFilter(f.classCode, f.subKey)) {
            continue;
        }
        if (!isLine && !f.lineGeometry && !isPoint &&
            polygonIdentityFilter &&
            !polygonIdentityFilter(f.classCode, f.subKey)) {
            continue;
        }
        // 区域按 kind 决定 scale(60/64/80 走 line-grid);
        // 其余类型 scale 与 kind 无关。
        // type2 content.#2 boundary lines use the line-grid scale even though
        // they remain in a type2 layer container.
        const int geometryLayerType =
            f.lineGeometry ? 1 : (f.polygonGeometry ? 2 : part.type);
        if (geometryLayerType == 3 &&
            !amapBuildingResolutionIsValid(part.z,
                                           f.buildingResolution)) {
            continue;
        }
        const double scale =
            f.coordScale > 0.0
                ? f.coordScale
                : amapCoordScale(geometryLayerType, part.z, f.kind,
                                 f.buildingResolution);
        // PoiLayer and TransitLayer points use the same official point
        // geometry contract. Their per-group resolution determines scale;
        // the enclosing type-4 layer must not force the line-grid scale.
        if (isPoint) {
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
                feat.properties["amap_subkey"] = std::to_string(f.subKey);
                feat.properties["amap_type"] = std::to_string(part.type);
                feat.properties["amap_zoom"] = std::to_string(part.z);
                if (f.hasDrawOrder) {
                    feat.properties["amap_draworder"] =
                        std::to_string(f.drawOrder);
                }
                publishOfficialWindow(feat);
                feat.properties["amap_rank"] = std::to_string(f.rank);
                feat.properties["amap_uid"] = std::to_string(f.uid);
                if (!f.name.empty()) {
                    feat.properties["name"] = f.name;
                }
                feat.labelSplitIndicesUtf16 = f.nameSplitIndicesUtf16;
                out.push_back(std::move(feat));
            }
            continue;
        }
        if (isLine || f.lineGeometry) {
            if (lineIdentityFilter &&
                !lineIdentityFilter(f.classCode, f.subKey)) {
                continue;
            }
            for (const auto& ring : f.rings) {
                if (f.roadNameGeometry && !f.shield.empty()) {
                    if (f.shieldType <= 0 || ring.size() < 2) continue;
                    const auto officialAnchor = amapOfficialRoadShieldAnchor(
                        part.x, part.y, part.z, scale, ring);
                    if (!officialAnchor) continue;
                    Cartographic c = *officialAnchor;
                    if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                    Feature shield;
                    shield.type = GeometryType::Point;
                    shield.rings = {{{c}}};
                    shield.properties["amap_class"] = "40001";
                    shield.properties["amap_subkey"] =
                        std::to_string(f.shieldType);
                    shield.properties["amap_type"] =
                        std::to_string(part.type);
                    shield.properties["amap_zoom"] = std::to_string(part.z);
                    if (f.hasDrawOrder) {
                        shield.properties["amap_draworder"] =
                            std::to_string(f.drawOrder);
                    }
                    publishOfficialWindow(shield);
                    shield.properties["amap_rank"] = std::to_string(f.rank);
                    shield.properties["name"] = f.shield;
                    out.push_back(std::move(shield));
                    continue;
                }
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
                feat.properties["amap_zoom"] = std::to_string(part.z);
                if (f.hasDrawOrder) {
                    feat.properties["amap_draworder"] =
                        std::to_string(f.drawOrder);
                }
                publishOfficialWindow(feat);
                if (f.kind > 0) {
                    feat.properties["amap_kind"] = std::to_string(f.kind);
                }
                // Preserve the official source line subKey for every line.
                feat.properties["amap_subkey"] = std::to_string(f.subKey);
                if (f.roadNameGeometry) {
                    feat.properties["amap_rank"] = std::to_string(f.rank);
                    if (!f.name.empty()) {
                        feat.properties["name"] = f.name;
                    }
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
                feat.properties["amap_subkey"] = std::to_string(f.subKey);
                feat.properties["amap_type"] = std::to_string(part.type);
                feat.properties["amap_zoom"] = std::to_string(part.z);
                feat.properties["amap_building_resolution"] =
                    std::to_string(f.buildingResolution);
                publishOfficialWindow(feat);
                if (f.hasDrawOrder) {
                    feat.properties["amap_draworder"] =
                        std::to_string(f.drawOrder);
                }
                if (f.kind > 0) {
                    feat.properties["amap_kind"] = std::to_string(f.kind);
                }
                // Preserve the official field value and its protobuf default
                // even when it is non-positive. The building tessellator then
                // fails that feature closed; dropping the property here would
                // incorrectly revive the generic planar polygon path.
                feat.properties["amap_height"] = std::to_string(f.height);
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
        auto groups = amapNormalizeEvenOddWinding(flippedRings);
        // normalizeEvenOddWinding deliberately keeps a lone ring byte-for-byte
        // compatible with the decoded input. In the no-clip fast path, however,
        // a negative lone ring is an independent outer ring, not a hole with a
        // missing parent. Orient it here so the outer+holes grouping below does
        // not silently drop a valid one-ring surface.
        if (groups.size() == 1 && ringSignedArea(groups.front()) < 0.0) {
            std::reverse(groups.front().begin(), groups.front().end());
        }
        constexpr double kClipMinX = -256.0;
        constexpr double kClipMaxX = 8192.0 + 256.0;
        constexpr double kClipMinY = -256.0;
        constexpr double kClipMaxY = 4096.0 + 256.0;
        bool outsideClipWindow = false;
        for (const auto& ring : flippedRings) {
            for (const auto& pt : ring) {
                outsideClipWindow =
                    outsideClipWindow || pt.first < kClipMinX ||
                    pt.first > kClipMaxX || pt.second < kClipMinY ||
                    pt.second > kClipMaxY;
            }
        }

        // If every normalized point is already inside the buffered tile window,
        // clipping is a no-op regardless of ring count/kind. Preserve the
        // outer+hole groups directly. The old unconditional `rings>2` branch
        // ran a full CDT here, emitted triangle pieces, then made the render
        // tessellator run CDT over those pieces again; dense z3 region tiles
        // spent seconds in each pass despite needing no clip at all.
        // Detailed multi-ring masks must keep one global even-odd solve.
        // Production z12 vegetation can carry more than one hundred touching
        // or nested rings; splitting that one source mask into outer+hole
        // Features makes each Feature tessellate independently and can fill
        // across neighbouring fragments.  Keeping every normalized ring in a
        // single Feature preserves the source modulo-two contract while still
        // doing only the render tessellator's one CDT pass.
        const bool needsWholeMaskSolve =
            isRegion && f.kind > 0 && part.z > 3 && groups.size() > 1;
        if (isRegion && !outsideClipWindow) {
            // Kind surfaces already normalize into outer-followed-by-holes
            // groups. Preserve those groups directly; the tessellator's
            // modulo-two constraint handling resolves their shared seams
            // without the extra triangulate/union pass used by legacy kind=0
            // compound masks.
            const size_t featureGroupLimit =
                needsWholeMaskSolve ? groups.size() : size_t{0};
            for (size_t gi = 0; gi < groups.size();) {
                if (ringSignedArea(groups[gi]) <= 0.0) {
                    ++gi;
                    continue;
                }
                Feature feat;
                feat.type = GeometryType::Polygon;
                for (size_t ri = gi; ri < groups.size(); ++ri) {
                    if (!needsWholeMaskSolve && ri > gi &&
                        ringSignedArea(groups[ri]) > 0.0) {
                        break;
                    }
                    const auto& clipped = groups[ri];
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
                    feat.properties["amap_zoom"] = std::to_string(part.z);
                    feat.properties["amap_kind"] = std::to_string(f.kind);
                    feat.properties["amap_subkey"] =
                        std::to_string(f.subKey);
                    if (f.hasDrawOrder) {
                        feat.properties["amap_draworder"] =
                            std::to_string(f.drawOrder);
                    }
                    publishOfficialWindow(feat);
                    out.push_back(std::move(feat));
                }
                if (featureGroupLimit != 0) {
                    gi = featureGroupLimit;
                    continue;
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

        if (isRegion && outsideClipWindow) {
            // 先对原始 even-odd 约束整体 CDT,再对每个凸三角形做窗口
            // 裁剪。这样凹面与窗口相交产生多个分量时不会生成隐式桥边。
            const auto clippedLoops = triangulateThenClipPolygon(
                flippedRings, kClipMinX, kClipMaxX, kClipMinY, kClipMaxY);
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
                feat.properties["amap_zoom"] = std::to_string(part.z);
                if (f.kind > 0) {
                    feat.properties["amap_kind"] = std::to_string(f.kind);
                }
                feat.properties["amap_subkey"] = std::to_string(f.subKey);
                if (f.hasDrawOrder) {
                    feat.properties["amap_draworder"] =
                        std::to_string(f.drawOrder);
                }
                publishOfficialWindow(feat);
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
            // 裁剪结果直接交给 PolygonTessellator，后者按 modulo 隐式
            // 闭合。这里不能再沿窗口边界补一条
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
            feat.properties["amap_zoom"] = std::to_string(part.z);
            if (f.kind > 0) {
                feat.properties["amap_kind"] = std::to_string(f.kind);
            }
            // regionBlocks(30002)与 30001 都只使用官方 subKey 调色板；
            // kind 只用于几何分类，缺失 identity 不再合成本地 block。
            feat.properties["amap_subkey"] = std::to_string(f.subKey);
            if (f.hasDrawOrder) {
                feat.properties["amap_draworder"] =
                    std::to_string(f.drawOrder);
            }
            publishOfficialWindow(feat);
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
#if defined(EARTH_ENGINE_TESTING)
std::vector<Feature> amapDecodedPartToFeatures(
    const AmapDecodedLayerPart& part, bool toWgs84) {
    return AmapClassicSourceBundle::Impl::convertPart(part, toWgs84);
}
#endif

}  // namespace earth_engine
