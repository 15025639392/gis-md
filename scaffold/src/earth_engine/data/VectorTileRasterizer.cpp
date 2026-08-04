#include "VectorTileRasterizer.h"

#include <algorithm>
#include <cmath>

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

VectorRasterImage rasterizeMvtTile(const MvtTile& tile, int zoom,
                                   const VectorRasterStyle& style, int size) {
    VectorRasterImage out;
    if (size <= 0) return out;
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
    for (const VectorRasterLayerPaint& paint : style.layers) {
        if (zoom < paint.minZoom || zoom > paint.maxZoom) continue;
        const MvtLayer* layer = nullptr;
        for (const MvtLayer& l : tile.layers) {
            if (l.name == paint.layer) { layer = &l; break; }
        }
        if (!layer) continue;
        const double scale =
            static_cast<double>(hi) / static_cast<double>(std::max(1u, layer->extent));

        // fill 与 line 各自一遍:同一层内先铺面再压线(线在面之上),而两者
        // 各自用**独立的覆盖 mask** —— 合用一个会让线色被面色覆盖。
        for (int pass = 0; pass < 2; ++pass) {
            const std::array<uint8_t, 4>& color =
                pass == 0 ? paint.fillColor : paint.lineColor;
            if (color[3] == 0) continue;
            canvas.reset(hi);
            edges.clear();
            for (const MvtFeature& feature : layer->features) {
                if (paint.filter &&
                    !paint.filter->matches(&feature.properties, zoom)) {
                    continue;
                }
                if (pass == 0) {
                    if (feature.type != MvtGeomType::Polygon) continue;
                    // 环分类沿用解码器那套(绕向自适应),孔环靠 nonzero
                    // 的反向绕向自动挖掉,不必显式区分。
                    for (const auto& ring : feature.paths) {
                        for (size_t i = 0; i + 1 < ring.size(); ++i) {
                            addEdge(edges, ring[i].x * scale, ring[i].y * scale,
                                    ring[i + 1].x * scale, ring[i + 1].y * scale);
                        }
                        if (ring.size() >= 3) {
                            addEdge(edges, ring.back().x * scale,
                                    ring.back().y * scale, ring.front().x * scale,
                                    ring.front().y * scale);
                        }
                    }
                } else {
                    if (feature.type != MvtGeomType::LineString &&
                        feature.type != MvtGeomType::Polygon) {
                        continue;
                    }
                    const double halfWidth =
                        std::max(0.5, paint.lineWidthPixels * ss * 0.5);
                    for (const auto& path : feature.paths) {
                        std::vector<std::pair<double, double>> pts;
                        pts.reserve(path.size());
                        for (const MvtPoint& p : path) {
                            pts.emplace_back(p.x * scale, p.y * scale);
                        }
                        if (feature.type == MvtGeomType::Polygon &&
                            pts.size() >= 3) {
                            pts.push_back(pts.front());  // 环闭合
                        }
                        strokePathEdges(pts, halfWidth, edges);
                    }
                }
            }
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

} // namespace earth_engine
