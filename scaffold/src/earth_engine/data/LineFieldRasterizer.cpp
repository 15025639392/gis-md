#include "LineFieldRasterizer.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

/// 把一条线段(纹素坐标)按 reach=halfWidth+feather 膨胀的 bbox 内逐纹素
/// 写 min(有符号边缘距离编码)。scatter 的核心。
void stampSegment(double x0, double y0, double x1, double y1,
                  double halfWidth, int size, std::vector<double>& sd) {
    const double reach = halfWidth + kLineFieldFeatherTexels;
    const int bx0 = std::max(0, static_cast<int>(
                                    std::floor(std::min(x0, x1) - reach)));
    const int bx1 = std::min(size - 1, static_cast<int>(std::ceil(
                                           std::max(x0, x1) + reach)));
    const int by0 = std::max(0, static_cast<int>(
                                    std::floor(std::min(y0, y1) - reach)));
    const int by1 = std::min(size - 1, static_cast<int>(std::ceil(
                                           std::max(y0, y1) + reach)));
    if (bx0 > bx1 || by0 > by1) return;
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    for (int py = by0; py <= by1; ++py) {
        const double cy = py + 0.5;
        for (int px = bx0; px <= bx1; ++px) {
            const double cx = px + 0.5;
            double t = 0.0;
            if (len2 > 1e-12) {
                t = ((cx - x0) * dx + (cy - y0) * dy) / len2;
                t = std::clamp(t, 0.0, 1.0);
            }
            const double qx = x0 + t * dx - cx;
            const double qy = y0 + t * dy - cy;
            const double dist = std::sqrt(qx * qx + qy * qy) - halfWidth;
            double& cell = sd[static_cast<size_t>(py) * size + px];
            cell = std::min(cell, dist);
        }
    }
}

} // namespace

LineFieldImage rasterizeLineFieldRect(const std::vector<MvtTileRef>& tiles,
                                      const MercatorRect& rect, int styleZoom,
                                      const VectorRasterStyle& style, int size,
                                      const UnitTransform* toTargetUnit) {
    LineFieldImage out;
    if (size <= 0) return out;
    const double rectW = rect.x1 - rect.x0;
    const double rectH = rect.y1 - rect.y0;
    if (!(rectW > 0.0) || !(rectH > 0.0)) return out;

    // 有符号距离缓冲(texel 单位),+∞ 语义用一个足够大的哨兵。
    std::vector<double> sd(static_cast<size_t>(size) * size,
                           kLineFieldFeatherTexels * 4.0);

    for (const VectorRasterLayerPaint& paint : style.layers) {
        if (paint.lineColor[3] == 0) continue;  // 只消费 line 通道
        if (styleZoom < paint.minZoom || styleZoom > paint.maxZoom) continue;
        const double halfWidth = std::max(0.25, paint.lineWidthPixels * 0.5);
        const double reach = halfWidth + kLineFieldFeatherTexels;

        for (const MvtTileRef& ref : tiles) {
            if (ref.tile == nullptr || ref.z < 0) continue;
            const MvtLayer* layer = nullptr;
            for (const MvtLayer& l : ref.tile->layers) {
                if (l.name == paint.layer) { layer = &l; break; }
            }
            if (!layer) continue;
            // 逐顶点:本地 → WGS84 unit →(可选)目标 unit → 画布(见
            // VectorTileRasterizer 的 UnitTransform;整页仿射对 GCJ 大页发散)。
            const double invExtent =
                1.0 / static_cast<double>(std::max(1u, layer->extent));
            const double tileSpan = 1.0 / static_cast<double>(1 << ref.z);
            const double refX = static_cast<double>(ref.x);
            const double refY = static_cast<double>(ref.y);
            auto mapPoint = [&](double lx, double ly, double& px, double& py) {
                double u = refX * tileSpan + lx * invExtent * tileSpan;
                double v = refY * tileSpan + ly * invExtent * tileSpan;
                if (toTargetUnit) (*toTargetUnit)(u, v);
                px = (u - rect.x0) / rectW * size;
                py = (v - rect.y0) / rectH * size;
            };

            for (const MvtFeature& feature : layer->features) {
                if (feature.type != MvtGeomType::LineString &&
                    feature.type != MvtGeomType::Polygon) {
                    continue;
                }
                if (paint.filter &&
                    !paint.filter->matches(&feature.properties, styleZoom)) {
                    continue;
                }
                for (const auto& path : feature.paths) {
                    if (path.size() < 2) continue;
                    // 剔除:path bbox 4 角 mapPoint → 画布 bbox(膨胀 reach)
                    // 不相交 → 整条跳过(GCJ 保序,4 角保守界定)。
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
                    double bx0 = cx[0], bx1 = cx[0], by0 = cy[0], by1 = cy[0];
                    for (int k = 1; k < 4; ++k) {
                        bx0 = std::min(bx0, cx[k]);
                        bx1 = std::max(bx1, cx[k]);
                        by0 = std::min(by0, cy[k]);
                        by1 = std::max(by1, cy[k]);
                    }
                    if (bx1 < -reach || bx0 > size + reach || by1 < -reach ||
                        by0 > size + reach) {
                        continue;
                    }
                    const bool closeRing =
                        feature.type == MvtGeomType::Polygon &&
                        path.size() >= 3;
                    const size_t segCount =
                        path.size() - 1 + (closeRing ? 1 : 0);
                    for (size_t i = 0; i < segCount; ++i) {
                        const MvtPoint& p0 = path[i];
                        const MvtPoint& p1 = path[(i + 1) % path.size()];
                        double ax, ay, bx, by;
                        mapPoint(p0.x, p0.y, ax, ay);
                        mapPoint(p1.x, p1.y, bx, by);
                        stampSegment(ax, ay, bx, by, halfWidth, size, sd);
                    }
                }
            }
        }
    }

    // 量化:0.5 = 线边缘,窗口 ±kFeatherTexels。
    out.size = size;
    out.r8.resize(sd.size());
    const double invWindow = 1.0 / (2.0 * kLineFieldFeatherTexels);
    for (size_t i = 0; i < sd.size(); ++i) {
        const double v = 0.5 - sd[i] * invWindow;
        out.r8[i] = static_cast<uint8_t>(
            std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    }
    return out;
}

} // namespace earth_engine
