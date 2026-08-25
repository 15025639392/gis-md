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

    // 高德瓦片环是**开放**的(瓦片边界裁剪产物)。**不能在这里直线补闭合点**:
    // 角点碎片(瓦片角 + 远处簇)直线闭合会自交,CDT/earcut 都会溢出
    // (实测 2-19×/1.89M×)。面积/点在环内判定天然按首尾环绕语义计算,
    // 开放环也能算;闭合交给后续 Sutherland-Hodgman 裁剪(沿瓦窗口闭合)。
    std::vector<std::vector<std::pair<double, double>>> closed = rings;
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

std::vector<std::pair<double, double>> amapCloseRingAlongWindow(
    std::vector<std::pair<double, double>> ring,
    double winMinX, double winMaxX, double winMinY, double winMaxY) {
    if (ring.size() < 3) return {};
    const auto& a = ring.front();
    const auto& b = ring.back();
    if (a.first == b.first && a.second == b.second) return ring;  // 已闭合

    const double span =
        std::max({winMaxX - winMinX, winMaxY - winMinY, 1.0});
    const double eps = 1e-9 * span;
    auto onEdge = [&](const std::pair<double, double>& p) {
        return std::abs(p.first - winMinX) <= eps ||
               std::abs(p.first - winMaxX) <= eps ||
               std::abs(p.second - winMinY) <= eps ||
               std::abs(p.second - winMaxY) <= eps;
    };
    if (!onEdge(a) || !onEdge(b)) {
        // 首尾都在窗口内:完全在窗口内的开放碎片,直线补首点;是否退化
        // 由调用方的 ratio 过滤判定。
        ring.push_back(a);
        return ring;
    }

    // 窗口角点 CCW:左下 → 右下 → 右上 → 左上。
    const std::array<std::pair<double, double>, 4> corners = {{
        {winMinX, winMinY}, {winMaxX, winMinY},
        {winMaxX, winMaxY}, {winMinX, winMaxY}}};
    auto edgeId = [&](const std::pair<double, double>& p) -> int {
        if (std::abs(p.second - winMinY) <= eps) return 0;  // bottom
        if (std::abs(p.first - winMaxX) <= eps) return 1;   // right
        if (std::abs(p.second - winMaxY) <= eps) return 2;  // top
        return 3;                                            // left
    };
    const int ea = edgeId(a);
    const int eb = edgeId(b);
    if (ea == eb) {
        // 同一边上的两点:直线连接即窗沿段,等价于沿边界闭合。
        ring.push_back(a);
        return ring;
    }
    auto boundaryPath = [&](int delta) {
        std::vector<std::pair<double, double>> out;
        int i = eb;
        while (true) {
            i = (i + delta + 4) % 4;
            out.push_back(corners[static_cast<size_t>(i)]);
            if (i == ea) break;
        }
        out.push_back(a);
        return out;
    };
    auto closedA = ring;
    for (const auto& p : boundaryPath(+1)) closedA.push_back(p);
    auto closedB = ring;
    for (const auto& p : boundaryPath(-1)) closedB.push_back(p);
    // 两条候选路径分别与环组成两个互补的闭合环(一条走角,一条绕其余
    // 三边)。选择与环自身绕向同号者:归一化后外环 area>0(CCW)、孔
    // area<0(CW),闭合不得改变填充侧。
    const double s = ringSignedArea(ring);
    return (ringSignedArea(closedA) > 0.0) == (s >= 0.0) ? closedA
                                                         : closedB;
}

std::vector<Feature> amapDecodedPartToFeatures(
    const AmapDecodedLayerPart& part, bool toWgs84) {
    std::vector<Feature> out;
    const bool isLine = part.type == 1 || part.type == 4;
    for (const auto& f : part.features) {
        // type2 区域按 kind 决定 scale(大区域 60/80 走 line-grid);
        // 其余类型 scale 与 kind 无关。
        const double scale = amapCoordScale(part.type, part.z, f.kind);
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
                    pt.second * scale);
                if (toWgs84) c = Gcj02CoordinateTransform::toWgs84(c);
                feat.rings = {{{c}}};
                feat.properties["amap_class"] = std::to_string(f.classCode);
                feat.properties["amap_subkey"] = std::to_string(f.subKey);
                feat.properties["amap_rank"] = std::to_string(f.rank);
                feat.properties["amap_minzoom"] = std::to_string(f.minZoom);
                feat.properties["amap_maxzoom"] = std::to_string(f.maxZoom);
                if (!f.name.empty()) {
                    feat.properties["name"] = f.name;
                }
                out.push_back(std::move(feat));
            }
            continue;
        }
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
            // 裁剪窗口 = **±256 buffer**([-256,8448]×[-256,4352]),与参考
            // 实现 xinzhi-map / 高德 JSAPI 的 POLY_CLIP_BUFFER=256 一致。
            // 高德瓦片面按瓦分块,相邻瓦的水体块在接缝处不一定精确重合
            // (源数据留 buffer 就是为了容忍这个):精确瓦窗会把跨缝块
            // 裁到缝上,块与块之间露出缝;buffer 让块延伸进邻瓦覆盖接缝。
            // 同色 fill 在带内重叠不可见;水/绿地与陆地底色异色时带内
            // 重叠由绘制顺序/深度解决(与参考 2D 合成同构)。
            std::vector<std::pair<double, double>> src;
            if (part.type == 2) {
                const auto clipped =
                    amapClipPolygonRing(ring, -256.0, 8192.0 + 256.0,
                                        -256.0, 4096.0 + 256.0);
                if (clipped.size() < 3) return pts;  // 窗外无幸存
                src = amapCloseRingAlongWindow(clipped, -256.0,
                                               8192.0 + 256.0, -256.0,
                                               4096.0 + 256.0);
            } else {
                src = ring;
            }
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
            // 退化开放碎片(完全在裁剪窗口内、裁剪后仍开放):toCarto 返回空,
            // 丢弃。若它是外环,先结束当前分组(避免后续孔错误并入)。
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
                    const auto holeMid = testPt(carto);
                    insideOuter =
                        pointInPoly(outer, holeMid.first, holeMid.second);
                }
                if (insideOuter) {
                    pending.push_back(std::move(carto));  // 孔,加入当前外环
                } else {
                    // 独立碎片(负绕向但互不嵌套):作为独立外环。
                    flush();
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
            // type2 面:粗源(z10 大掩膜)与主源(z12-14 细地块/水/绿地)都要。
            // 主源缺 type2 = 30002 城市地块层整层消失,粗源绿地/水系大掩膜
            // 裸露成整片绿色(与 amap.com 观感不符,spec fl-30002/fl-water/
            // fl-green 都是 main 源)。
            // [1:1 坐标空间] amap.com 网页在 GCJ-02 空间渲染;我们此前把数据
            // GCJ→WGS84 转换后上 WGS84 球,同一相机数字下多边形整体偏移
            // ~400m(≈屏幕 1/3),形状与网页对不上。改为保留 GCJ 原生坐标。
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
