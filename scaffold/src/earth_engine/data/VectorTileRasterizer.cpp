#include "VectorTileRasterizer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace earth_engine {

namespace {

/// 超采样缓冲里的一条边(已换算成超采样像素坐标)。
struct Edge {
    double x0, y0, x1, y1;
    int winding;  // +1 / -1,供 nonzero 填充规则
};

/// 单像素画布(超采样分辨率)。累计颜色用直接覆盖而非混合:同一图层内部
/// 自重叠(如相邻路段)不该叠出更深的颜色,那会在每个接头处留下暗斑。
/// 图层之间才做 alpha 混合(见 blendLayer)。
struct Canvas {
    int size = 0;
    std::vector<uint8_t> mask;  // 0/1 覆盖标记

    void reset(int s) {
        size = s;
        mask.assign(static_cast<size_t>(s) * s, 0);
    }
};

void addEdge(std::vector<Edge>& edges, double x0, double y0, double x1,
             double y1) {
    if (y0 == y1) return;  // 水平边对扫描线无贡献
    edges.push_back(y0 < y1 ? Edge{x0, y0, x1, y1, +1}
                            : Edge{x1, y1, x0, y0, -1});
}

/// nonzero winding 扫描线填充。写进 mask(1 = 覆盖)。
///
/// 用 nonzero 而非 even-odd:MVT 的孔环绕向与外环相反,nonzero 天然把孔挖掉;
/// even-odd 在自相交多边形上会挖出错误的洞(OSM 数据里自相交并不罕见)。
void fillEdges(const std::vector<Edge>& edges, Canvas& canvas) {
    if (edges.empty()) return;
    double minY = edges[0].y0, maxY = edges[0].y1;
    for (const Edge& e : edges) {
        minY = std::min(minY, e.y0);
        maxY = std::max(maxY, e.y1);
    }
    const int yBegin = std::max(0, static_cast<int>(std::floor(minY)));
    const int yEnd = std::min(canvas.size - 1, static_cast<int>(std::ceil(maxY)));

    struct Hit { double x; int winding; };
    std::vector<Hit> hits;
    for (int y = yBegin; y <= yEnd; ++y) {
        const double sy = y + 0.5;  // 扫描线取像素中心,避免顶点恰在整数行时重复计数
        hits.clear();
        for (const Edge& e : edges) {
            if (sy < e.y0 || sy >= e.y1) continue;
            const double t = (sy - e.y0) / (e.y1 - e.y0);
            hits.push_back({e.x0 + t * (e.x1 - e.x0), e.winding});
        }
        if (hits.size() < 2) continue;
        std::sort(hits.begin(), hits.end(),
                  [](const Hit& a, const Hit& b) { return a.x < b.x; });
        int winding = 0;
        for (size_t i = 0; i + 1 < hits.size(); ++i) {
            winding += hits[i].winding;
            if (winding == 0) continue;
            const int xs = std::max(0, static_cast<int>(std::ceil(hits[i].x - 0.5)));
            const int xe = std::min(canvas.size - 1,
                                    static_cast<int>(std::floor(hits[i + 1].x - 0.5)));
            uint8_t* row = canvas.mask.data() + static_cast<size_t>(y) * canvas.size;
            for (int x = xs; x <= xe; ++x) row[x] = 1;
        }
    }
}

/// 折线描边:每段挤成一个四边形,接头处补一个方形 —— 方形接头比 miter 省事
/// 且在小线宽下不可辨,而 miter 在锐角处会甩出长刺(OSM 路网急弯不少)。
void strokePathEdges(const std::vector<std::pair<double, double>>& pts,
                     double halfWidth, std::vector<Edge>& edges) {
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const double x0 = pts[i].first, y0 = pts[i].second;
        const double x1 = pts[i + 1].first, y1 = pts[i + 1].second;
        const double dx = x1 - x0, dy = y1 - y0;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-9) continue;
        const double nx = -dy / len * halfWidth;
        const double ny = dx / len * halfWidth;
        // 四边形按固定绕向发射,保证 nonzero 填充恒非零(段间自重叠无害)。
        addEdge(edges, x0 + nx, y0 + ny, x1 + nx, y1 + ny);
        addEdge(edges, x1 + nx, y1 + ny, x1 - nx, y1 - ny);
        addEdge(edges, x1 - nx, y1 - ny, x0 - nx, y0 - ny);
        addEdge(edges, x0 - nx, y0 - ny, x0 + nx, y0 + ny);
    }
    // 接头方块(含首尾端点,等价 square cap)。
    // ⚠️ 绕向必须与上面的线段四边形**一致**:nonzero 规则下反向绕的重叠区
    // 会 +1 + (−1) = 0 被挖空 —— 表现为每个路口一个缺口,而"路口有洞"极难
    // 反推到绕向。判据见 SelfOverlapDoesNotDarken。
    for (const auto& p : pts) {
        const double h = halfWidth;
        addEdge(edges, p.first - h, p.second + h, p.first + h, p.second + h);
        addEdge(edges, p.first + h, p.second + h, p.first + h, p.second - h);
        addEdge(edges, p.first + h, p.second - h, p.first - h, p.second - h);
        addEdge(edges, p.first - h, p.second - h, p.first - h, p.second + h);
    }
}

/// 把一层的覆盖 mask 以给定颜色混进目标(超采样分辨率的 RGBA)。
void blendLayer(const Canvas& canvas, const std::array<uint8_t, 4>& color,
                std::vector<uint8_t>& target) {
    const double srcA = color[3] / 255.0;
    if (srcA <= 0.0) return;
    for (size_t i = 0; i < canvas.mask.size(); ++i) {
        if (!canvas.mask[i]) continue;
        uint8_t* px = target.data() + i * 4;
        for (int c = 0; c < 3; ++c) {
            px[c] = static_cast<uint8_t>(
                std::lround(color[c] * srcA + px[c] * (1.0 - srcA)));
        }
        const double dstA = px[3] / 255.0;
        px[3] = static_cast<uint8_t>(
            std::lround((srcA + dstA * (1.0 - srcA)) * 255.0));
    }
}

} // namespace

VectorRasterImage rasterizeMvtRect(const std::vector<MvtTileRef>& tiles,
                                   const MercatorRect& rect, int styleZoom,
                                   const VectorRasterStyle& style, int size,
                                   const UnitTransform* toTargetUnit) {
    VectorRasterImage out;
    if (size <= 0) return out;
    const double rectW = rect.x1 - rect.x0;
    const double rectH = rect.y1 - rect.y0;
    if (!(rectW > 0.0) || !(rectH > 0.0)) return out;
    const int ss = std::clamp(style.supersample, 1, 4);
    const int hi = size * ss;

    std::vector<uint8_t> buffer(static_cast<size_t>(hi) * hi * 4);
    for (size_t i = 0; i < buffer.size(); i += 4) {
        buffer[i + 0] = style.background[0];
        buffer[i + 1] = style.background[1];
        buffer[i + 2] = style.background[2];
        buffer[i + 3] = style.background[3];
    }

    Canvas canvas;
    std::vector<Edge> edges;
    std::vector<std::pair<double, double>> pts;
    for (const VectorRasterLayerPaint& paint : style.layers) {
        if (styleZoom < paint.minZoom || styleZoom > paint.maxZoom) continue;

        // fill 与 line 各自一遍:同一层内先铺面再压线(线在面之上),而两者
        // 各自用**独立的覆盖 mask** —— 合用一个会让线色被面色覆盖。
        for (int pass = 0; pass < 2; ++pass) {
            const std::array<uint8_t, 4>& color =
                pass == 0 ? paint.fillColor : paint.lineColor;
            if (color[3] == 0) continue;
            canvas.reset(hi);
            edges.clear();
            bool any = false;
            for (const MvtTileRef& ref : tiles) {
                if (ref.tile == nullptr || ref.z < 0) continue;
                const MvtLayer* layer = nullptr;
                for (const MvtLayer& l : ref.tile->layers) {
                    if (l.name == paint.layer) { layer = &l; break; }
                }
                if (!layer) continue;
                // 逐顶点:本地坐标 → WGS84 unit → (可选)目标采样空间 unit →
                // 画布像素。整页仿射预乘对 GCJ 大页/祖先页边缘发散上百米(见
                // UnitTransform 注释),故逐点变换;标准 overlay(null)退化线性。
                const double invExtent =
                    1.0 / static_cast<double>(std::max(1u, layer->extent));
                const double tileSpan = 1.0 / static_cast<double>(1 << ref.z);
                const double refX = static_cast<double>(ref.x);
                const double refY = static_cast<double>(ref.y);
                auto mapPoint = [&](double lx, double ly, double& px,
                                    double& py) {
                    double u = refX * tileSpan + lx * invExtent * tileSpan;
                    double v = refY * tileSpan + ly * invExtent * tileSpan;
                    if (toTargetUnit) (*toTargetUnit)(u, v);
                    px = (u - rect.x0) / rectW * hi;
                    py = (v - rect.y0) / rectH * hi;
                };
                // 剔除:path bbox 4 角 mapPoint → 画布 bbox(GCJ 保序,4 角保守
                // 界定所有点)。不相交 ⇒ 跳过恒安全(同旧 pathTouchesCanvas)。
                auto pathTouches = [&](const std::vector<MvtPoint>& path,
                                       double pad) -> bool {
                    if (path.empty()) return false;
                    double mnx = path[0].x, mxx = path[0].x;
                    double mny = path[0].y, mxy = path[0].y;
                    for (const MvtPoint& p : path) {
                        mnx = std::min(mnx, static_cast<double>(p.x));
                        mxx = std::max(mxx, static_cast<double>(p.x));
                        mny = std::min(mny, static_cast<double>(p.y));
                        mxy = std::max(mxy, static_cast<double>(p.y));
                    }
                    double cx[4], cy[4];
                    mapPoint(mnx, mny, cx[0], cy[0]);
                    mapPoint(mxx, mny, cx[1], cy[1]);
                    mapPoint(mnx, mxy, cx[2], cy[2]);
                    mapPoint(mxx, mxy, cx[3], cy[3]);
                    double px0 = cx[0], px1 = cx[0], py0 = cy[0], py1 = cy[0];
                    for (int k = 1; k < 4; ++k) {
                        px0 = std::min(px0, cx[k]);
                        px1 = std::max(px1, cx[k]);
                        py0 = std::min(py0, cy[k]);
                        py1 = std::max(py1, cy[k]);
                    }
                    return px1 + pad >= 0.0 &&
                           px0 - pad <= static_cast<double>(hi) &&
                           py1 + pad >= 0.0 &&
                           py0 - pad <= static_cast<double>(hi);
                };

                const double halfWidth =
                    std::max(0.5, paint.lineWidthPixels * ss * 0.5);
                for (const MvtFeature& feature : layer->features) {
                    if (paint.filter &&
                        !paint.filter->matches(&feature.properties,
                                               styleZoom)) {
                        continue;
                    }
                    if (pass == 0) {
                        if (feature.type != MvtGeomType::Polygon) continue;
                        // 环分类沿用解码器那套(绕向自适应),孔环靠 nonzero
                        // 的反向绕向自动挖掉,不必显式区分。
                        for (const auto& ring : feature.paths) {
                            if (!pathTouches(ring, 0.0)) continue;
                            any = true;
                            double ax, ay, bx, by;
                            for (size_t i = 0; i + 1 < ring.size(); ++i) {
                                mapPoint(ring[i].x, ring[i].y, ax, ay);
                                mapPoint(ring[i + 1].x, ring[i + 1].y, bx, by);
                                addEdge(edges, ax, ay, bx, by);
                            }
                            if (ring.size() >= 3) {
                                mapPoint(ring.back().x, ring.back().y, ax, ay);
                                mapPoint(ring.front().x, ring.front().y, bx,
                                         by);
                                addEdge(edges, ax, ay, bx, by);
                            }
                        }
                    } else {
                        if (feature.type != MvtGeomType::LineString &&
                            feature.type != MvtGeomType::Polygon) {
                            continue;
                        }
                        for (const auto& path : feature.paths) {
                            if (!pathTouches(path, halfWidth)) continue;
                            any = true;
                            pts.clear();
                            pts.reserve(path.size());
                            double px, py;
                            for (const MvtPoint& p : path) {
                                mapPoint(p.x, p.y, px, py);
                                pts.emplace_back(px, py);
                            }
                            if (feature.type == MvtGeomType::Polygon &&
                                pts.size() >= 3) {
                                pts.push_back(pts.front());  // 环闭合
                            }
                            strokePathEdges(pts, halfWidth, edges);
                        }
                    }
                }
            }
            if (!any) continue;
            fillEdges(edges, canvas);
            blendLayer(canvas, color, buffer);
        }
    }

    // 盒式降采样 = 覆盖率抗锯齿。
    out.size = size;
    out.rgba.assign(static_cast<size_t>(size) * size * 4, 0);
    const int samples = ss * ss;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < ss; ++sy) {
                const uint8_t* row =
                    buffer.data() +
                    (static_cast<size_t>(y * ss + sy) * hi + x * ss) * 4;
                for (int sx = 0; sx < ss; ++sx) {
                    for (int c = 0; c < 4; ++c) acc[c] += row[sx * 4 + c];
                }
            }
            uint8_t* dst =
                out.rgba.data() + (static_cast<size_t>(y) * size + x) * 4;
            for (int c = 0; c < 4; ++c) {
                dst[c] = static_cast<uint8_t>(acc[c] / samples);
            }
        }
    }
    return out;
}

VectorRasterImage rasterizeMvtTile(const MvtTile& tile, int zoom,
                                   const VectorRasterStyle& style, int size) {
    // 单瓦整图 = 矩形版的特例:把瓦片放在 z=0 原点,矩形取整个 unit 平面。
    // 坐标数学与 E4 版逐位等价(scale = hi/extent,offset = 0)。
    std::vector<MvtTileRef> tiles{MvtTileRef{&tile, 0, 0, 0}};
    return rasterizeMvtRect(tiles, MercatorRect{0.0, 0.0, 1.0, 1.0}, zoom,
                            style, size);
}

namespace {

constexpr double kPiRaster = 3.14159265358979323846;

double unitXFromLng(double lngRad) { return lngRad / (2.0 * kPiRaster) + 0.5; }

double unitYFromLat(double latRad) {
    return 0.5 -
           std::log(std::tan(kPiRaster / 4.0 + latRad / 2.0)) /
               (2.0 * kPiRaster);
}

bool featureMatchesPaint(const Feature& feature,
                         const VectorRasterLayerPaint& paint, int styleZoom) {
    if (feature.type != GeometryType::Polygon) return false;
    if (paint.filter &&
        !paint.filter->matches(&feature.properties, styleZoom)) {
        return false;
    }
    if (paint.layer.empty() || paint.layer == "*") return true;
    auto hit = [&](const char* key) {
        const auto it = feature.properties.find(key);
        return it != feature.properties.end() && it->second == paint.layer;
    };
    return hit("amap_fillkey") || hit("mvt_layer");
}

}  // namespace

VectorRasterImage rasterizeFeaturePolygonsRect(
    const std::vector<const Feature*>& features, const MercatorRect& rect,
    int styleZoom, const VectorRasterStyle& style, int size,
    const UnitTransform* toTargetUnit) {
    VectorRasterImage out;
    if (size <= 0) return out;
    const double rectW = rect.x1 - rect.x0;
    const double rectH = rect.y1 - rect.y0;
    if (!(rectW > 0.0) || !(rectH > 0.0)) return out;
    const int ss = std::clamp(style.supersample, 1, 4);
    const int hi = size * ss;

    std::vector<uint8_t> buffer(static_cast<size_t>(hi) * hi * 4);
    for (size_t i = 0; i < buffer.size(); i += 4) {
        buffer[i + 0] = style.background[0];
        buffer[i + 1] = style.background[1];
        buffer[i + 2] = style.background[2];
        buffer[i + 3] = style.background[3];
    }

    auto mapPoint = [&](const Cartographic& c, double& px, double& py) {
        double u = unitXFromLng(c.longitude());
        double v = unitYFromLat(c.latitude());
        if (toTargetUnit) (*toTargetUnit)(u, v);
        px = (u - rect.x0) / rectW * hi;
        py = (v - rect.y0) / rectH * hi;
    };

    auto ringTouches = [&](const std::vector<Cartographic>& ring,
                           double pad) -> bool {
        if (ring.empty()) return false;
        double px0 = 0, px1 = 0, py0 = 0, py1 = 0;
        bool first = true;
        for (const Cartographic& c : ring) {
            double px, py;
            mapPoint(c, px, py);
            if (first) {
                px0 = px1 = px;
                py0 = py1 = py;
                first = false;
            } else {
                px0 = std::min(px0, px);
                px1 = std::max(px1, px);
                py0 = std::min(py0, py);
                py1 = std::max(py1, py);
            }
        }
        return px1 + pad >= 0.0 &&
               px0 - pad <= static_cast<double>(hi) &&
               py1 + pad >= 0.0 &&
               py0 - pad <= static_cast<double>(hi);
    };

    Canvas canvas;
    std::vector<Edge> edges;
    for (const VectorRasterLayerPaint& paint : style.layers) {
        if (styleZoom < paint.minZoom || styleZoom > paint.maxZoom) continue;
        const std::array<uint8_t, 4>& color = paint.fillColor;
        if (color[3] == 0) continue;
        canvas.reset(hi);
        edges.clear();
        bool any = false;
        for (const Feature* feature : features) {
            if (!feature || !featureMatchesPaint(*feature, paint, styleZoom)) {
                continue;
            }
            for (const auto& ring : feature->rings) {
                if (ring.size() < 3 || !ringTouches(ring, 0.0)) continue;
                any = true;
                double ax, ay, bx, by;
                for (size_t i = 0; i + 1 < ring.size(); ++i) {
                    mapPoint(ring[i], ax, ay);
                    mapPoint(ring[i + 1], bx, by);
                    addEdge(edges, ax, ay, bx, by);
                }
                const Cartographic& last = ring.back();
                const Cartographic& firstPt = ring.front();
                if (last.longitude() != firstPt.longitude() ||
                    last.latitude() != firstPt.latitude()) {
                    mapPoint(last, ax, ay);
                    mapPoint(firstPt, bx, by);
                    addEdge(edges, ax, ay, bx, by);
                }
            }
        }
        if (!any) continue;
        fillEdges(edges, canvas);
        blendLayer(canvas, color, buffer);
    }

    out.size = size;
    out.rgba.assign(static_cast<size_t>(size) * size * 4, 0);
    const int samples = ss * ss;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < ss; ++sy) {
                const uint8_t* row =
                    buffer.data() +
                    (static_cast<size_t>(y * ss + sy) * hi + x * ss) * 4;
                for (int sx = 0; sx < ss; ++sx) {
                    for (int c = 0; c < 4; ++c) acc[c] += row[sx * 4 + c];
                }
            }
            uint8_t* dst =
                out.rgba.data() + (static_cast<size_t>(y) * size + x) * 4;
            for (int c = 0; c < 4; ++c) {
                dst[c] = static_cast<uint8_t>(acc[c] / samples);
            }
        }
    }
    return out;
}

} // namespace earth_engine
