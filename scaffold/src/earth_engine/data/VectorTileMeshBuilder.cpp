#include "VectorTileMeshBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "ConstrainedDelaunay.h"

namespace earth_engine {

namespace {

using Edge = ConstrainedDelaunay::Edge;

/// 整数瓦片坐标去重键。MVT 坐标本就是整数,直接打包成 64 位无精度损失 ——
/// CDT 的前置约定是「输入点两两不同」,重合点会让它退化出空结果。
uint64_t packPoint(const MvtPoint& p) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(p.x)) << 32) |
           static_cast<uint64_t>(static_cast<uint32_t>(p.y));
}

void pushVertex(VectorTileMesh& mesh, double x, double y, double ex, double ey,
                const std::array<uint8_t, 4>& color) {
    VectorTileMeshVertex v;
    v.x = static_cast<float>(x);
    v.y = static_cast<float>(y);
    v.ex = static_cast<float>(ex);
    v.ey = static_cast<float>(ey);
    v.r = color[0];
    v.g = color[1];
    v.b = color[2];
    v.a = color[3];
    mesh.vertices.push_back(v);
}

void pushQuad(VectorTileMesh& mesh, uint32_t base) {
    // base..base+3 = (起点+n, 起点-n, 终点+n, 终点-n)。不开背面剔除,绕向无关。
    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 3);
    mesh.indices.push_back(base + 2);
}

/// 一个要素的全部环 → CDT → 三角形。环不分外/孔:CDT 按约束边奇偶 flood-fill
/// 标记内外,孔自动挖掉(与 E4-1 栅格路径「靠 nonzero 反向绕向自动挖」等价,
/// 两条路径对同一份数据给同样的内外判定)。
void appendPolygonFill(VectorTileMesh& mesh, const MvtFeature& feature,
                       double invExtent,
                       const std::array<uint8_t, 4>& color) {
    std::vector<glm::dvec2> points;
    std::vector<Edge> edges;
    std::unordered_map<uint64_t, uint32_t> lookup;

    auto indexOf = [&](const MvtPoint& p) -> uint32_t {
        const uint64_t key = packPoint(p);
        auto it = lookup.find(key);
        if (it != lookup.end()) {
            return it->second;
        }
        const uint32_t idx = static_cast<uint32_t>(points.size());
        points.emplace_back(static_cast<double>(p.x) * invExtent,
                            static_cast<double>(p.y) * invExtent);
        lookup.emplace(key, idx);
        return idx;
    };

    for (const std::vector<MvtPoint>& ring : feature.paths) {
        if (ring.size() < 3) {
            continue;  // 退化环:不足以围出面积
        }
        for (size_t i = 0; i < ring.size(); ++i) {
            const uint32_t a = indexOf(ring[i]);
            const uint32_t b = indexOf(ring[(i + 1) % ring.size()]);  // 隐式闭合
            if (a != b) {
                edges.emplace_back(a, b);
            }
        }
    }
    if (points.size() < 3 || edges.empty()) {
        return;
    }
    const std::vector<uint32_t> tris =
        ConstrainedDelaunay::triangulate(points, edges);
    if (tris.empty()) {
        return;
    }
    const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
    for (const glm::dvec2& p : points) {
        pushVertex(mesh, p.x, p.y, 0.0, 0.0, color);
    }
    for (uint32_t idx : tris) {
        mesh.indices.push_back(base + idx);
    }
}

/// 一条折线 → 逐段四边形 + 逐拐点方块接头。挤压量存进顶点(单位 = 页像素),
/// 位置不动 —— 网格因此与页 zoom 无关。
void appendStrokedPath(VectorTileMesh& mesh,
                       const std::vector<glm::dvec2>& pts, double halfWidth,
                       const std::array<uint8_t, 4>& color) {
    if (pts.size() < 2 || halfWidth <= 0.0) {
        return;
    }
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const glm::dvec2& p0 = pts[i];
        const glm::dvec2& p1 = pts[i + 1];
        const double dx = p1.x - p0.x;
        const double dy = p1.y - p0.y;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0) {
            continue;  // 零长段:法线无定义
        }
        const double nx = -dy / len * halfWidth;
        const double ny = dx / len * halfWidth;
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        pushVertex(mesh, p0.x, p0.y, nx, ny, color);
        pushVertex(mesh, p0.x, p0.y, -nx, -ny, color);
        pushVertex(mesh, p1.x, p1.y, nx, ny, color);
        pushVertex(mesh, p1.x, p1.y, -nx, -ny, color);
        pushQuad(mesh, base);
    }
    // 方块接头(含端点):比 miter 稳健 —— miter 在锐角处会甩出长刺(E4-1 同因
    // 选的方形)。首尾也放,等价于 square cap。
    for (const glm::dvec2& p : pts) {
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        pushVertex(mesh, p.x, p.y, -halfWidth, -halfWidth, color);
        pushVertex(mesh, p.x, p.y, halfWidth, -halfWidth, color);
        pushVertex(mesh, p.x, p.y, -halfWidth, halfWidth, color);
        pushVertex(mesh, p.x, p.y, halfWidth, halfWidth, color);
        pushQuad(mesh, base);
    }
}

}  // namespace

VectorTileMesh buildVectorTileMesh(const MvtTile& tile, int zoom,
                                   const VectorRasterStyle& style) {
    VectorTileMesh mesh;
    std::vector<glm::dvec2> scratchPts;

    for (const VectorRasterLayerPaint& paint : style.layers) {
        if (zoom < paint.minZoom || zoom > paint.maxZoom) {
            continue;
        }
        const MvtLayer* layer = nullptr;
        for (const MvtLayer& l : tile.layers) {
            if (l.name == paint.layer) {
                layer = &l;
                break;
            }
        }
        if (!layer) {
            continue;
        }
        const double invExtent = 1.0 / static_cast<double>(std::max(1u, layer->extent));

        // 层内两遍:先面后线(线压在面上)。与 E4-1 栅格路径同序 —— 两条路径
        // 必须给出同样的压盖关系,否则 cell 在页存储与 directComposite 之间切换时
        // 画面会跳。
        for (int pass = 0; pass < 2; ++pass) {
            const std::array<uint8_t, 4>& color =
                pass == 0 ? paint.fillColor : paint.lineColor;
            if (color[3] == 0) {
                continue;  // alpha=0 = 不绘制该通道(不是画透明)
            }
            for (const MvtFeature& feature : layer->features) {
                if (paint.filter &&
                    !paint.filter->matches(&feature.properties, zoom)) {
                    continue;
                }
                if (pass == 0) {
                    if (feature.type != MvtGeomType::Polygon) {
                        continue;
                    }
                    appendPolygonFill(mesh, feature, invExtent, color);
                    continue;
                }
                if (feature.type != MvtGeomType::LineString &&
                    feature.type != MvtGeomType::Polygon) {
                    continue;
                }
                const double halfWidth =
                    std::max(0.5, paint.lineWidthPixels * 0.5);
                for (const std::vector<MvtPoint>& path : feature.paths) {
                    scratchPts.clear();
                    scratchPts.reserve(path.size() + 1);
                    for (const MvtPoint& p : path) {
                        scratchPts.emplace_back(
                            static_cast<double>(p.x) * invExtent,
                            static_cast<double>(p.y) * invExtent);
                    }
                    if (feature.type == MvtGeomType::Polygon &&
                        scratchPts.size() >= 3) {
                        scratchPts.push_back(scratchPts.front());  // 环闭合
                    }
                    appendStrokedPath(mesh, scratchPts, halfWidth, color);
                }
            }
        }
    }
    return mesh;
}

}  // namespace earth_engine
