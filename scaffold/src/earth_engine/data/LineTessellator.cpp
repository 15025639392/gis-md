#include "LineTessellator.h"

#include "../core/geodesy/Cartographic.h"
#include "../core/geodesy/Ellipsoid.h"

#include <cmath>

namespace earth_engine {

namespace {

double dist3(const Vec3& a, const Vec3& b) {
    const double dx = a.x() - b.x(), dy = a.y() - b.y(), dz = a.z() - b.z();
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

TessellatedLine LineTessellator::tessellate(const Feature& feature,
                                            const Ellipsoid& ellipsoid,
                                            double heightOffset, bool closed) {
    TessellatedLine out;
    if (feature.type != GeometryType::LineString || feature.rings.empty()) {
        return out;
    }
    const auto& ring = feature.rings[0];
    if (ring.size() < 2) return out;

    // 转 ECEF + 去连续重合点。
    std::vector<Vec3> pos;
    pos.reserve(ring.size());
    for (const auto& c : ring) {
        Vec3 p = ellipsoid.cartographicToCartesian(
            Cartographic(c.longitude(), c.latitude(), c.height() + heightOffset));
        if (!pos.empty() && dist3(pos.back(), p) == 0.0) continue;
        pos.push_back(p);
    }
    // closed 时若首尾重合,丢弃末点(环绕由 wrap 提供)。
    if (closed && pos.size() >= 2 && dist3(pos.front(), pos.back()) == 0.0) {
        pos.pop_back();
    }
    const size_t n = pos.size();
    if (n < 2) return out;

    // 累计弧长。
    std::vector<double> lengthSoFar(n, 0.0);
    for (size_t i = 1; i < n; ++i)
        lengthSoFar[i] = lengthSoFar[i - 1] + dist3(pos[i - 1], pos[i]);

    // prev/next(端点哨兵 = 自身;closed 环绕)。
    auto prevOf = [&](size_t i) -> const Vec3& {
        if (i > 0) return pos[i - 1];
        return closed ? pos[n - 1] : pos[0];
    };
    auto nextOf = [&](size_t i) -> const Vec3& {
        if (i + 1 < n) return pos[i + 1];
        return closed ? pos[0] : pos[n - 1];
    };

    // 每折线顶点 2 个 ribbon 顶点(side +1 / -1)。
    out.vertices.reserve(2 * n);
    for (size_t i = 0; i < n; ++i) {
        for (float side : {1.0f, -1.0f}) {
            LineVertex v;
            v.pos = pos[i];
            v.prev = prevOf(i);
            v.next = nextOf(i);
            v.side = side;
            v.lengthSoFar = static_cast<float>(lengthSoFar[i]);
            out.vertices.push_back(v);
        }
    }

    // 段 → 2 三角形。open: n-1 段;closed: n 段(末段 n-1→0)。
    const size_t segCount = closed ? n : n - 1;
    out.indices.reserve(6 * segCount);
    for (size_t s = 0; s < segCount; ++s) {
        const auto a = static_cast<uint32_t>(2 * s);          // 顶点 s, side +1
        const auto aN = static_cast<uint32_t>(a + 1);         // 顶点 s, side -1
        const size_t jn = (s + 1) % n;
        const auto b = static_cast<uint32_t>(2 * jn);         // 顶点 s+1, side +1
        const auto bN = static_cast<uint32_t>(b + 1);         // 顶点 s+1, side -1
        // 两三角形(a, aN, b) (b, aN, bN)。
        out.indices.push_back(a);
        out.indices.push_back(aN);
        out.indices.push_back(b);
        out.indices.push_back(b);
        out.indices.push_back(aN);
        out.indices.push_back(bN);
    }
    return out;
}

} // namespace earth_engine
