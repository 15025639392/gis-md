#include "LineFieldRasterizer.h"

#include <algorithm>
#include <cmath>

namespace earth_engine {

namespace {

/// 每纹素的"最近线段"scatter 记录(编码前的全精度中间态)。
struct TexelRecord {
    double dist = 1e18;  // 到最近线段的距离(texel)
    double ox = 0.0, oy = 0.0;    // 最近点 − 纹素中心
    double ux = 0.0, uy = 0.0;    // 线段方向(已规范化:uy>0 或 uy==0&&ux>0)
    double fwd = 0.0, back = 0.0; // 最近点到段两端的剩余长度(沿规范化方向)
};

/// 把一条线段按 kLineFieldOffsetRangeTexels 膨胀的 bbox 内逐纹素写
/// min-dist 记录(距离胜者的段参数整套落纹素)。scatter 的核心。
void stampSegment(double x0, double y0, double x1, double y1, int size,
                  std::vector<TexelRecord>& recs) {
    const double reach = kLineFieldOffsetRangeTexels;
    const int bx0 = std::max(0, static_cast<int>(
                                    std::floor(std::min(x0, x1) - reach)));
    const int bx1 = std::min(size - 1, static_cast<int>(std::ceil(
                                           std::max(x0, x1) + reach)));
    const int by0 = std::max(0, static_cast<int>(
                                    std::floor(std::min(y0, y1) - reach)));
    const int by1 = std::min(size - 1, static_cast<int>(std::ceil(
                                           std::max(y0, y1) + reach)));
    if (bx0 > bx1 || by0 > by1) return;
    double dx = x1 - x0;
    double dy = y1 - y0;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-9) return;  // 退化微段:无方向可言,跳过
    double ux = dx / len, uy = dy / len;
    // 方向规范化(θ∈[0,π));fwd/back 在规范化方向下定义,翻转须同步换端。
    if (uy < 0.0 || (uy == 0.0 && ux < 0.0)) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        ux = -ux;
        uy = -uy;
    }
    for (int py = by0; py <= by1; ++py) {
        const double cy = py + 0.5;
        for (int px = bx0; px <= bx1; ++px) {
            const double cx = px + 0.5;
            double t = ((cx - x0) * ux + (cy - y0) * uy);
            t = std::clamp(t, 0.0, len);
            const double qx = x0 + t * ux;
            const double qy = y0 + t * uy;
            const double ddx = qx - cx;
            const double ddy = qy - cy;
            const double dist = std::sqrt(ddx * ddx + ddy * ddy);
            TexelRecord& rec = recs[static_cast<size_t>(py) * size + px];
            if (dist < rec.dist) {
                rec.dist = dist;
                rec.ox = ddx;
                rec.oy = ddy;
                rec.ux = ux;
                rec.uy = uy;
                rec.fwd = len - t;
                rec.back = t;
            }
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

    std::vector<TexelRecord> recs(static_cast<size_t>(size) * size);

    for (const VectorRasterLayerPaint& paint : style.layers) {
        if (paint.lineColor[3] == 0) continue;  // 只消费 line 通道
        if (styleZoom < paint.minZoom || styleZoom > paint.maxZoom) continue;
        const double reach = kLineFieldOffsetRangeTexels;

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
                        stampSegment(ax, ay, bx, by, size, recs);
                    }
                }
            }
        }
    }

    // 编码(语义见头文件):全 0 = 空哨兵/失败安全。
    out.size = size;
    out.rgba8.assign(recs.size() * 4u, 0);
    constexpr double kOff = kLineFieldOffsetRangeTexels;
    constexpr double kCla = kLineFieldClampMaxTexels;
    constexpr double kStep = kLineFieldClampStepTexels;
    for (size_t i = 0; i < recs.size(); ++i) {
        const TexelRecord& rec = recs[i];
        if (rec.dist > kOff) continue;
        uint8_t* px = out.rgba8.data() + i * 4u;
        px[0] = static_cast<uint8_t>(
            std::lround((std::clamp(rec.ox, -kOff, kOff) / kOff * 0.5 + 0.5) *
                        255.0));
        px[1] = static_cast<uint8_t>(
            std::lround((std::clamp(rec.oy, -kOff, kOff) / kOff * 0.5 + 0.5) *
                        255.0));
        const double theta = std::atan2(rec.uy, rec.ux);  // 规范化 → [0,π)
        px[2] = static_cast<uint8_t>(
            std::clamp<long>(std::lround(theta / 3.14159265358979 * 255.0),
                             0, 255));
        const int fl = static_cast<int>(std::clamp<long>(
            std::lround(std::min(rec.fwd, kCla) / kStep), 0, 15));
        const int bl = static_cast<int>(std::clamp<long>(
            std::lround(std::min(rec.back, kCla) / kStep), 0, 15));
        int packed = (fl << 4) | bl;
        if (packed == 0) packed = 1;  // A==0 是空哨兵;退化微段提升 1 级
        px[3] = static_cast<uint8_t>(packed);
    }
    return out;
}

} // namespace earth_engine
