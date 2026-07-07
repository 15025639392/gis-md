#include "PolygonTessellator.h"

#include "ConstrainedDelaunay.h"
#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"

#include <cmath>
#include <cstdint>
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

} // namespace

TessellatedFill PolygonTessellator::tessellate(const Feature& feature,
                                               const Ellipsoid& ellipsoid,
                                               double heightOffset) {
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

    if (points2D.size() < 3 || constraints.empty()) return out;

    std::vector<uint32_t> tris =
        ConstrainedDelaunay::triangulate(points2D, constraints);
    if (tris.empty()) return out;

    // 唯一点 → ECEF 顶点。
    out.positions.reserve(uniqueCart.size());
    for (const auto& c : uniqueCart) {
        Cartographic ch(c.longitude(), c.latitude(), c.height() + heightOffset);
        out.positions.push_back(ellipsoid.cartographicToCartesian(ch));
    }
    out.fillIndices = std::move(tris);
    out.outlineIndices = std::move(outline);
    return out;
}

} // namespace earth_engine
