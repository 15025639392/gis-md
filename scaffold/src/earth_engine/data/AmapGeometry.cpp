#include "AmapGeometry.h"

#include "../core/geodesy/Gcj02CoordinateTransform.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

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
std::pair<double, double> ringInteriorPoint(
    const std::vector<std::pair<double, double>>& ring,
    const RingBounds& bounds) {
    const double spanY = bounds.maxY - bounds.minY;
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
            if (bestW >= 0.0) return {bestMid, y};
        }
    }
    return ring.empty() ? std::make_pair(0.0, 0.0) : ring[0];
}

}  // namespace

bool amapRegionUsesLineGrid(int regionKind) {
    return regionKind == 60 || regionKind == 64 || regionKind == 80;
}

double amapCoordScale(int layerType, int layerZ, int regionKind) {
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

std::vector<std::vector<std::pair<double, double>>> amapNormalizeEvenOddWinding(
    const std::vector<std::vector<std::pair<double, double>>>& rings) {
    const size_t n = rings.size();

    // 高德瓦片环是**开放**的(瓦片边界裁剪产物):面积/点在环内判定、
    // 后续 CDT 三角化都要求闭合。先补闭合点(首点 != 尾点时追加首点)。
    std::vector<std::vector<std::pair<double, double>>> closed = rings;
    for (auto& ring : closed) {
        if (ring.size() >= 3 &&
            !(ring.front() == ring.back())) {
            ring.push_back(ring.front());
        }
    }
    if (n <= 1) return closed;

    std::vector<double> areas(n);
    std::vector<double> absAreas(n);
    std::vector<RingBounds> bounds(n);
    std::vector<std::pair<double, double>> interior(n);
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
        const double px = interior[i].first, py = interior[i].second;
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

    // 无被包围环 → 每个环都是外环,nonzero 填充本来正确,原样返回。
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
    // 兜底:嵌套分析退化时绝不丢环。
    if (out.size() < n) {
        for (size_t i = 0; i < n; ++i) {
            if (depth[i] % 2 == 1 && parent[i] == -1) {
                out.push_back(closed[i]);
            }
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
    const bool isLine = part.type == 1 || part.type == 4;
    for (const auto& f : part.features) {
        // type2 区域按 kind 决定 scale(大区域 60/80 走 line-grid);
        // 其余类型 scale 与 kind 无关。
        const double scale = amapCoordScale(part.type, part.z, f.kind);
        if (isLine) {
            for (const auto& ring : f.rings) {
                Feature feat;
                feat.type = GeometryType::LineString;
                std::vector<Cartographic> pts;
                pts.reserve(ring.size());
                for (const auto& pt : ring) {
                    Cartographic c = amapTileLocalToLngLat(
                        part.x, part.y, part.z, pt.first * scale,
                        pt.second * scale);
                    if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                    pts.push_back(c);
                }
                feat.rings.push_back(std::move(pts));
                feat.properties["amap_class"] = std::to_string(f.classCode);
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
                        pt.second * scale);
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

        // type2 区域:环全部同向(even-odd 掩膜),归一化为
        // 「外环 + 孔环」分组,每组一个 Polygon(三角化自动挖孔)。
        // 归一化保证:外环 area>0,孔 area<0,且每个外环后紧跟它的孔
        // (下一个外环之前)。消费端按绕向符号分组即可,无需重算嵌套。
        //
        // ⚠️ 归一化必须与 CDT 同一坐标语义:高德 tile-local y 底朝上,
        // 经纬度 lat 是翻转后的 y(amapTileLocalToLngLat flipY=true)。
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
        const auto toCarto = [&](const auto& ring) {
            std::vector<Cartographic> pts;
            // 瓦片裁剪仅用于 type2 区域(跨边界大掩膜/条带);type3 建筑
            // footprint 在瓦片内,裁剪会破坏庭院孔(孔盖外环 → 空 fill)。
            const bool doClip = part.type == 2;
            const auto& src = doClip
                ? amapClipPolygonRing(ring, -256.0, 8192.0 + 256.0,
                                      -256.0, 4096.0 + 256.0)
                : ring;
            pts.reserve(src.size());
            for (const auto& pt : src) {
                // 归一化/裁剪在「翻转后」坐标(与 lat 同号);toCarto 传回
                // flipY=true 的 amapTileLocalToLngLat,还原 y 底朝上
                // (裁剪坐标已含 scale,还原即 kAmapExtentY - py)。
                Cartographic c = amapTileLocalToLngLat(
                    part.x, part.y, part.z, pt.first,
                    kAmapExtentY - pt.second);
                if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                pts.push_back(c);
            }
            // 裁剪后环在窗口边闭合;若仍开放(首尾不同)补首点。
            if (pts.size() >= 3 &&
                !(pts.front().longitude() == pts.back().longitude() &&
                  pts.front().latitude() == pts.back().latitude())) {
                pts.push_back(pts.front());
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
            if (f.kind > 0) {
                feat.properties["amap_kind"] = std::to_string(f.kind);
            }
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
            const double area = ringSignedArea(ring);
            if (area > 0.0) {
                flush();  // 新外环 → 结束上一分组
                pending.push_back(toCarto(ring));
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
                    // pending 存的是 Cartographic(经纬度),包含测试要用
                    // 同一坐标;经纬度 lat 与 canonical y 同号,直接用。
                    auto testPt = [&](const std::vector<Cartographic>& r) {
                        double mnx = 1e18, mxx = -1e18, mny = 1e18,
                               mxy = -1e18;
                        for (const auto& c : r) {
                            mnx = std::min(mnx, c.longitude());
                            mxx = std::max(mxx, c.longitude());
                            mny = std::min(mny, c.latitude());
                            mxy = std::max(mxy, c.latitude());
                        }
                        return std::make_pair((mnx + mxx) * 0.5,
                                              (mny + mxy) * 0.5);
                    };
                    const auto mid = testPt(outer);
                    // 射线法点在环内(经纬度坐标,与 ring 同符号语义)。
                    auto pointInPoly = [](const std::vector<Cartographic>& r,
                                          double px, double py) {
                        bool inside = false;
                        const size_t n = r.size();
                        for (size_t i = 0, j = n - 1; i < n; j = i++) {
                            const double xi = r[i].longitude();
                            const double yi = r[i].latitude();
                            const double xj = r[j].longitude();
                            const double yj = r[j].latitude();
                            if (((yi > py) != (yj > py)) &&
                                (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
                                inside = !inside;
                            }
                        }
                        return inside;
                    };
                    // 孔自身的中点必须在外环内(孔 ⊂ 外环)。
                    const auto holeMid = testPt(toCarto(ring));
                    insideOuter =
                        pointInPoly(outer, holeMid.first, holeMid.second);
                }
                if (insideOuter) {
                    pending.push_back(toCarto(ring));  // 孔,加入当前外环
                } else {
                    // 独立碎片(负绕向但互不嵌套):作为独立外环。
                    flush();
                    pending.push_back(toCarto(ring));
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
                // 主源(z14):只保留 30001 层水系/绿地(kind 63/61);
                // 30002 地块与 30001 其他 kind 主源不画(地块走粗源浅灰)。
                AmapDecodedLayerPart kept = p;
                kept.features.clear();
                for (const auto& f : p.features) {
                    if (f.classCode == 30001 &&
                        (f.kind == 63 || f.kind == 61)) {
                        kept.features.push_back(f);
                    }
                }
                auto fs = amapDecodedPartToFeatures(kept, true);
                out.insert(out.end(), std::make_move_iterator(fs.begin()),
                           std::make_move_iterator(fs.end()));
            } else {
                auto fs = amapDecodedPartToFeatures(p, true);
                out.insert(out.end(), std::make_move_iterator(fs.begin()),
                           std::make_move_iterator(fs.end()));
            }
            continue;
        }
        // type1 线 / type3 建筑 / type4 轨道:主源才要。
        if (regionsOnly) continue;
        auto fs = amapDecodedPartToFeatures(p, true);
        out.insert(out.end(), std::make_move_iterator(fs.begin()),
                   std::make_move_iterator(fs.end()));
    }
    return true;
}

}  // namespace earth_engine
